// Native SM75 tensor issue-rate test. Targets only this machine's 2080 Ti.
// No management, clock, firmware or register writes.
#include <cuda_runtime.h>
#include <cuda_fp16.h>
#include <cublas_v2.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#define CUDA(x) do { cudaError_t e=(x); if(e!=cudaSuccess) { \
 fprintf(stderr,"%s:%d: %s: %s\n",__FILE__,__LINE__,#x,cudaGetErrorString(e)); exit(1); }} while(0)
#define BLAS(x) do { cublasStatus_t e=(x); if(e!=CUBLAS_STATUS_SUCCESS) { \
 fprintf(stderr,"%s:%d: %s: %d\n",__FILE__,__LINE__,#x,int(e)); exit(1); }} while(0)

__device__ __forceinline__ void mma32(float *c, unsigned a, unsigned b) {
 asm volatile("mma.sync.aligned.m16n8k8.row.col.f32.f16.f16.f32 "
 "{%0,%1,%2,%3}, {%4,%4}, {%5}, {%0,%1,%2,%3};"
 : "+f"(c[0]),"+f"(c[1]),"+f"(c[2]),"+f"(c[3]) : "r"(a),"r"(b));
}
__device__ __forceinline__ void mma16(unsigned *c, unsigned a, unsigned b) {
 asm volatile("mma.sync.aligned.m16n8k8.row.col.f16.f16.f16.f16 "
 "{%0,%1}, {%2,%2}, {%3}, {%0,%1};"
 : "+r"(c[0]),"+r"(c[1]) : "r"(a),"r"(b));
}

template<bool FP32> __global__ __launch_bounds__(1024,1)
void issue_rate(float *out, unsigned long long *ticks, unsigned *smids,
                int rounds, unsigned one, unsigned negone) {
 float cf[4][4] = {};
 unsigned ch[4][2] = {};
 __syncthreads();
 unsigned long long start=clock64();
 for(int r=0;r<rounds;r++) {
  #pragma unroll
  for(int u=0;u<8;u++) {
   #pragma unroll
   for(int j=0;j<4;j++) {
    if constexpr(FP32) mma32(cf[j],one,one); else mma16(ch[j],one,one);
   }
   #pragma unroll
   for(int j=0;j<4;j++) {
    if constexpr(FP32) mma32(cf[j],one,negone); else mma16(ch[j],one,negone);
   }
  }
 }
 // Non-zero exact-result check; every accumulator component must equal 8.
 #pragma unroll
 for(int j=0;j<4;j++) {
  if constexpr(FP32) mma32(cf[j],one,one); else mma16(ch[j],one,one);
 }
 __syncthreads();
 unsigned long long end=clock64();
 if(threadIdx.x==0) {
  ticks[blockIdx.x]=end-start;
  unsigned smid; asm("mov.u32 %0, %%smid;" : "=r"(smid));
  smids[blockIdx.x]=smid;
 }
 float error=0;
 #pragma unroll
 for(int j=0;j<4;j++) {
  #pragma unroll
  for(int k=0;k<4;k++) {
   float v;
   if constexpr(FP32) v=cf[j][k];
   else v=__half2float(__ushort_as_half((ch[j][k/2] >> (16*(k%2))) & 0xffff));
   error=fmaxf(error,fabsf(v-8.0f));
  }
 }
 out[blockIdx.x*blockDim.x+threadIdx.x]=error;
}

template<bool FP32> void run_issue(int sms) {
 constexpr int rounds=2048, threads=1024;
 float *out; unsigned long long *ticks; unsigned *smids;
 CUDA(cudaMalloc(&out,sms*threads*sizeof(float)));
 CUDA(cudaMalloc(&ticks,sms*sizeof(*ticks)));
 CUDA(cudaMalloc(&smids,sms*sizeof(*smids)));
 cudaEvent_t begin,end; CUDA(cudaEventCreate(&begin)); CUDA(cudaEventCreate(&end));
 std::vector<float> errors(sms*threads);
 std::vector<unsigned long long> cycles(sms);
 std::vector<unsigned> ids(sms);
 int active; CUDA(cudaOccupancyMaxActiveBlocksPerMultiprocessor(&active,issue_rate<FP32>,threads,0));
 if(active!=1) { fprintf(stderr,"Unexpected occupancy: %d\n",active); exit(1); }
 for(int sample=-2;sample<7;sample++) {
  CUDA(cudaEventRecord(begin));
  issue_rate<FP32><<<sms,threads>>>(out,ticks,smids,rounds,0x3c003c00,0xbc00bc00);
  CUDA(cudaGetLastError()); CUDA(cudaEventRecord(end)); CUDA(cudaEventSynchronize(end));
  float ms; CUDA(cudaEventElapsedTime(&ms,begin,end));
  CUDA(cudaMemcpy(errors.data(),out,errors.size()*sizeof(float),cudaMemcpyDeviceToHost));
  CUDA(cudaMemcpy(cycles.data(),ticks,cycles.size()*sizeof(*ticks),cudaMemcpyDeviceToHost));
  CUDA(cudaMemcpy(ids.data(),smids,ids.size()*sizeof(*smids),cudaMemcpyDeviceToHost));
  float maxerr=0; for(float v:errors) { if(!std::isfinite(v)) exit(2); maxerr=std::max(maxerr,v); }
  if(maxerr!=0) { fprintf(stderr,"Tensor exact-result failure: %g\n",maxerr); exit(2); }
  std::sort(cycles.begin(),cycles.end()); std::sort(ids.begin(),ids.end());
  size_t unique=std::unique(ids.begin(),ids.end())-ids.begin();
  if(unique!=size_t(sms)) { fprintf(stderr,"Not one CTA per SM: %zu/%d\n",unique,sms); exit(2); }
  double flops_per_sm=double(rounds*64+4)*32*2048;
  if(sample>=0) printf("{\"test\":\"mma_f16_acc_%s\",\"sample\":%d,\"ms\":%.6f,"
   "\"tflops\":%.4f,\"median_sm_cycles\":%llu,\"flops_per_sm_cycle\":%.4f,"
   "\"max_abs_error\":%.1f,\"unique_sms\":%zu}\n",
   FP32?"fp32":"fp16",sample,ms,flops_per_sm*sms/(ms*1e9),cycles[sms/2],
   flops_per_sm/cycles[sms/2],maxerr,unique);
 }
 CUDA(cudaEventDestroy(begin)); CUDA(cudaEventDestroy(end));
 CUDA(cudaFree(out)); CUDA(cudaFree(ticks)); CUDA(cudaFree(smids));
}

void gemm_check_and_bench() {
 cublasHandle_t h; BLAS(cublasCreate(&h));
 // Exact dyadic random-looking values, independent CPU reference, column major.
 constexpr int n=256;
 std::vector<__half> a(n*n),b(n*n); std::vector<float> got(n*n);
 for(int i=0;i<n*n;i++) { a[i]=__float2half(float((i*17+i/97)%15-7)/8);
                         b[i]=__float2half(float((i*11+i/71)%13-6)/8); }
 __half *da,*db; float *dc;
 CUDA(cudaMalloc(&da,n*n*sizeof(__half))); CUDA(cudaMalloc(&db,n*n*sizeof(__half)));
 CUDA(cudaMalloc(&dc,n*n*sizeof(float)));
 CUDA(cudaMemcpy(da,a.data(),a.size()*sizeof(__half),cudaMemcpyHostToDevice));
 CUDA(cudaMemcpy(db,b.data(),b.size()*sizeof(__half),cudaMemcpyHostToDevice));
 float alpha=1,beta=0;
 BLAS(cublasGemmEx(h,CUBLAS_OP_N,CUBLAS_OP_N,n,n,n,&alpha,da,CUDA_R_16F,n,
   db,CUDA_R_16F,n,&beta,dc,CUDA_R_32F,n,CUBLAS_COMPUTE_32F,CUBLAS_GEMM_DEFAULT_TENSOR_OP));
 CUDA(cudaMemcpy(got.data(),dc,got.size()*sizeof(float),cudaMemcpyDeviceToHost));
 double maxerr=0;
 for(int col=0;col<n;col++) for(int row=0;row<n;row++) {
  double expected=0;
  for(int k=0;k<n;k++) expected+=double(__half2float(a[row+k*n]))*__half2float(b[k+col*n]);
  if(!std::isfinite(got[row+col*n])) exit(2);
  maxerr=std::max(maxerr,std::abs(expected-got[row+col*n]));
 }
 printf("{\"test\":\"cublas_fp32_acc_cpu_reference\",\"n\":%d,\"max_abs_error\":%.9g}\n",n,maxerr);
 if(maxerr>1e-4) exit(2);
 CUDA(cudaFree(da)); CUDA(cudaFree(db)); CUDA(cudaFree(dc));
 constexpr int big=8192;
 CUDA(cudaMalloc(&da,size_t(big)*big*sizeof(__half))); CUDA(cudaMalloc(&db,size_t(big)*big*sizeof(__half)));
 CUDA(cudaMalloc(&dc,size_t(big)*big*sizeof(float)));
 CUDA(cudaMemset(da,0,size_t(big)*big*sizeof(__half))); CUDA(cudaMemset(db,0,size_t(big)*big*sizeof(__half)));
 cudaEvent_t start,stop; CUDA(cudaEventCreate(&start)); CUDA(cudaEventCreate(&stop));
 for(int sample=-2;sample<7;sample++) {
  CUDA(cudaEventRecord(start));
  BLAS(cublasGemmEx(h,CUBLAS_OP_N,CUBLAS_OP_N,big,big,big,&alpha,da,CUDA_R_16F,big,
    db,CUDA_R_16F,big,&beta,dc,CUDA_R_32F,big,CUBLAS_COMPUTE_32F,CUBLAS_GEMM_DEFAULT_TENSOR_OP));
  CUDA(cudaEventRecord(stop)); CUDA(cudaEventSynchronize(stop));
  float ms; CUDA(cudaEventElapsedTime(&ms,start,stop));
  if(sample>=0) printf("{\"test\":\"cublas_f16_acc_fp32\",\"n\":%d,\"sample\":%d,\"ms\":%.6f,\"tflops\":%.4f}\n",
    big,sample,ms,2.0*big*big*big/(ms*1e9));
 }
 CUDA(cudaEventDestroy(start)); CUDA(cudaEventDestroy(stop));
 CUDA(cudaFree(da)); CUDA(cudaFree(db)); CUDA(cudaFree(dc)); BLAS(cublasDestroy(h));
}

int main() {
 setvbuf(stdout,nullptr,_IOLBF,0);
 int device; CUDA(cudaDeviceGetByPCIBusId(&device,"0000:02:00.0"));
 cudaDeviceProp p; CUDA(cudaGetDeviceProperties(&p,device));
 if(p.major!=7 || p.minor!=5 || p.multiProcessorCount!=68) {
  fprintf(stderr,"Not the expected 68-SM SM75 GPU; refusing.\n"); return 1;
 }
 CUDA(cudaSetDevice(device));
 printf("{\"gpu\":\"%s\",\"bdf\":\"0000:02:00.0\",\"sms\":%d,\"sm\":75,\"threads_per_block\":1024,\"rounds\":2048}\n",p.name,p.multiProcessorCount);
 run_issue<true>(p.multiProcessorCount); run_issue<false>(p.multiProcessorCount);
 gemm_check_and_bench(); CUDA(cudaDeviceSynchronize()); return 0;
}
