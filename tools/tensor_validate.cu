// Post-unlock arithmetic validation, not firmware/control code.
// FP16 inputs are exact dyadics. Integer CPU dot products give an independent
// exact reference for these bounded FP32 accumulations (no tolerance hiding).
// The 8192 test samples 128 output coordinates, not the entire output matrix.
#include <cuda_runtime.h>
#include <cuda_fp16.h>
#include <cublas_v2.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
#define CUDA(x) do { cudaError_t e=(x); if(e!=cudaSuccess) { \
  fprintf(stderr,"%s: %s\n",#x,cudaGetErrorString(e));exit(1); }} while(0)
#define BLAS(x) do { cublasStatus_t e=(x); if(e!=CUBLAS_STATUS_SUCCESS) { \
  fprintf(stderr,"%s: status %d\n",#x,int(e));exit(1); }} while(0)

static int value(uint32_t i,uint32_t seed) {
  uint32_t x=i^seed;x^=x>>16;x*=0x7feb352du;x^=x>>15;x*=0x846ca68bu;x^=x>>16;
  return int(x%63)-31;
}
static void check(cublasHandle_t h,int m,int n,int k,bool ta,bool tb,
                  float alpha,float beta,uint32_t seed,bool large) {
  int ar=ta?k:m,ac=ta?m:k,br=tb?n:k,bc=tb?k:n;
  int lda=ar+(large?0:3),ldb=br+(large?0:5),ldc=m+(large?0:7);
  size_t as=size_t(lda)*ac,bs=size_t(ldb)*bc,cs=size_t(ldc)*n;
  std::vector<__half> a(as),b(bs);std::vector<float> c(cs),got(cs);
  for(size_t i=0;i<as;i++)a[i]=__float2half(value(uint32_t(i),seed)/16.0f);
  for(size_t i=0;i<bs;i++)b[i]=__float2half(value(uint32_t(i),seed^0x13579bdfu)/16.0f);
  for(size_t i=0;i<cs;i++)c[i]=value(uint32_t(i),seed^0x2468ace0u)/16.0f;
  __half *da,*db;float *dc;
  CUDA(cudaMalloc(&da,as*sizeof(*da)));CUDA(cudaMalloc(&db,bs*sizeof(*db)));
  CUDA(cudaMalloc(&dc,cs*sizeof(*dc)));
  CUDA(cudaMemcpy(da,a.data(),as*sizeof(*da),cudaMemcpyHostToDevice));
  CUDA(cudaMemcpy(db,b.data(),bs*sizeof(*db),cudaMemcpyHostToDevice));
  CUDA(cudaMemcpy(dc,c.data(),cs*sizeof(*dc),cudaMemcpyHostToDevice));
  auto gemm=[&]() {
    BLAS(cublasGemmEx(h,ta?CUBLAS_OP_T:CUBLAS_OP_N,tb?CUBLAS_OP_T:CUBLAS_OP_N,
      m,n,k,&alpha,da,CUDA_R_16F,lda,db,CUDA_R_16F,ldb,&beta,dc,CUDA_R_32F,ldc,
      CUBLAS_COMPUTE_32F,CUBLAS_GEMM_DEFAULT_TENSOR_OP));
  };
  gemm();CUDA(cudaDeviceSynchronize());
  CUDA(cudaMemcpy(got.data(),dc,cs*sizeof(*dc),cudaMemcpyDeviceToHost));
  size_t checks=large?128:size_t(m)*n;
  double maxerr=0;
  for(size_t q=0;q<checks;q++) {
    int row=large?int((q*61+17)%m):int(q%size_t(m));
    int col=large?int((q*127+29)%n):int(q/size_t(m));
    int64_t sum=0;
    for(int z=0;z<k;z++) {
      size_t ai=ta?size_t(z)+size_t(row)*lda:size_t(row)+size_t(z)*lda;
      size_t bi=tb?size_t(col)+size_t(z)*ldb:size_t(z)+size_t(col)*ldb;
      sum+=int64_t(value(uint32_t(ai),seed))*value(uint32_t(bi),seed^0x13579bdfu);
    }
    double expected=double(alpha)*(double(sum)/256.0)+double(beta)*c[row+size_t(col)*ldc];
    double actual=got[row+size_t(col)*ldc];
    if(!std::isfinite(actual)) { fprintf(stderr,"non-finite output\n");exit(2); }
    maxerr=std::max(maxerr,std::abs(actual-expected));
  }
  for(int col=0;col<n;col++)for(int row=m;row<ldc;row++)
    if(got[row+size_t(col)*ldc]!=c[row+size_t(col)*ldc]) {
      fprintf(stderr,"Output padding modified\n");exit(2);
    }
  printf("{\"test\":\"nonzero_exact_gemm\",\"m\":%d,\"n\":%d,\"k\":%d,"
    "\"transpose_a\":%d,\"transpose_b\":%d,\"alpha\":%.1f,\"beta\":%.1f,"
    "\"checked_outputs\":%zu,\"sampled\":%s,\"seed\":%u,\"max_abs_error\":%.9g}\n",
    m,n,k,ta,tb,alpha,beta,checks,large?"true":"false",seed,maxerr);
  if(maxerr!=0) { fprintf(stderr,"Exact CPU-reference mismatch\n");exit(2); }
  if(large) {
    // beta=0 is required so repeated timing launches use the same expression.
    if(beta!=0)exit(3);
    cudaEvent_t start,stop;CUDA(cudaEventCreate(&start));CUDA(cudaEventCreate(&stop));
    for(int sample=-2;sample<7;sample++) {
      CUDA(cudaEventRecord(start));gemm();CUDA(cudaEventRecord(stop));CUDA(cudaEventSynchronize(stop));
      float ms;CUDA(cudaEventElapsedTime(&ms,start,stop));
      if(sample>=0)printf("{\"test\":\"nonzero_gemm_timing\",\"sample\":%d,\"ms\":%.6f,\"tflops\":%.4f}\n",
        sample,ms,2.0*m*n*k/(ms*1e9));
    }
    CUDA(cudaEventDestroy(start));CUDA(cudaEventDestroy(stop));
  }
  CUDA(cudaFree(da));CUDA(cudaFree(db));CUDA(cudaFree(dc));
}
int main() {
  setvbuf(stdout,nullptr,_IOLBF,0);
  int d;CUDA(cudaDeviceGetByPCIBusId(&d,"0000:02:00.0"));cudaDeviceProp p;
  CUDA(cudaGetDeviceProperties(&p,d));
  if(p.major!=7 || p.minor!=5 || p.multiProcessorCount!=68) {
    fprintf(stderr,"Expected 68-SM SM75 GPU only\n");return 1;
  }
  CUDA(cudaSetDevice(d));cublasHandle_t h;BLAS(cublasCreate(&h));
  int runtime,driver,blas;CUDA(cudaRuntimeGetVersion(&runtime));CUDA(cudaDriverGetVersion(&driver));
  BLAS(cublasGetVersion(h,&blas));
  printf("{\"gpu\":\"%s\",\"sms\":%d,\"cuda_runtime\":%d,\"cuda_driver\":%d,\"cublas\":%d}\n",
    p.name,p.multiProcessorCount,runtime,driver,blas);
  for(int flags=0;flags<4;flags++) {
    check(h,128,128,128,flags&1,flags&2,1,0,0x12345678u+flags,false);
    check(h,127,193,257,flags&1,flags&2,-0.5f,0.5f,0x87654321u+flags,false);
  }
  check(h,511,257,383,false,false,1,0,0x10203040u,false);
  check(h,8192,8192,8192,false,false,1,0,0x24681357u,true);
  BLAS(cublasDestroy(h));CUDA(cudaDeviceSynchronize());return 0;
}
