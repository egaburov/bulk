#include <moderngpu.cuh>
#include <thrust/scan.h>
#include <thrust/fill.h>
#include <thrust/device_vector.h>
#include <thrust/functional.h>
#include <thrust/random.h>
#include <cassert>
#include <iostream>
#include "time_invocation_cuda.hpp"
#include <thrust/detail/temporary_array.h>
#include <thrust/copy.h>
#include <bulk/bulk.hpp>


template<unsigned int size, unsigned int grainsize>
struct inclusive_scan_n
{
  template<typename InputIterator, typename Size, typename OutputIterator, typename T, typename BinaryFunction>
  __device__ void operator()(bulk::static_thread_group<size,grainsize> &this_group, InputIterator first, Size n, OutputIterator result, T init, BinaryFunction binary_op)
  {
    bulk::inclusive_scan(this_group, first, first + n, result, init, binary_op);
  }
};


template<unsigned int size, unsigned int grainsize>
struct exclusive_scan_n
{
  template<typename InputIterator, typename Size, typename OutputIterator, typename T, typename BinaryFunction>
  __device__ void operator()(bulk::static_thread_group<size,grainsize> &this_group, InputIterator first, Size n, OutputIterator result, T init, BinaryFunction binary_op)
  {
    bulk::exclusive_scan(this_group, first, first + n, result, init, binary_op);
  }
};


template<std::size_t groupsize, std::size_t grainsize>
struct inclusive_downsweep
{
  template<typename RandomAccessIterator1, typename RandomAccessIterator2, typename T, typename BinaryFunction>
  __device__ void operator()(bulk::static_thread_group<groupsize,grainsize> &this_group,
                             RandomAccessIterator1 first,
                             int count,
                             int2 task,
                             const T *carries,
                             RandomAccessIterator2 result,
                             BinaryFunction binary_op)
  {
    const int elements_per_group = groupsize * grainsize;
  
    int2 range = mgpu::ComputeTaskRange(this_group.index(), task, elements_per_group, count);
  
    RandomAccessIterator1 last = first + range.y;
    first += range.x;
    result += range.x;
  
    bulk::inclusive_scan(this_group, first, last, result, carries[this_group.index()], binary_op);
  }
};


template<std::size_t groupsize, std::size_t grainsize>
struct reduce_tiles
{
  template<typename InputIterator, typename BinaryFunction>
  __device__ void operator()(bulk::static_thread_group<groupsize,grainsize> &this_group,
                             InputIterator first,
                             int n,
                             int2 task,
                             typename thrust::iterator_value<InputIterator>::type *reduction_global,
                             BinaryFunction binary_op)
  {
    typedef typename thrust::iterator_value<InputIterator>::type value_type;
    
    int2 range = mgpu::ComputeTaskRange(this_group.index(), task, groupsize * grainsize, n);

    // it's much faster to pass the last value as the init for some reason
    value_type total = bulk::reduce(this_group, first + range.x, first + range.y - 1, first[range.y-1], binary_op);

    if(this_group.this_thread.index() == 0)
    {
      reduction_global[this_group.index()] = total;
    } // end if
  } // end operator()
}; // end reduce_tiles


template<typename RandomAccessIterator1, typename RandomAccessIterator2, typename T, typename BinaryFunction>
RandomAccessIterator2 inclusive_scan(RandomAccessIterator1 first, size_t n, RandomAccessIterator2 result, T init, BinaryFunction binary_op, mgpu::CudaContext& context)
{
  // XXX TODO pass explicit heap sizes

  // XXX TODO infer intermediate type from iterators and BinaryFunction
  typedef typename thrust::iterator_value<RandomAccessIterator2>::type intermediate_type;
  
  const int threshold_of_parallelism = 20000;

  if(n < threshold_of_parallelism)
  {
    const int groupsize = 512;
    const int grainsize = 3;

    bulk::static_thread_group<groupsize,grainsize> group;
    bulk::async(bulk::par(group, 1), inclusive_scan_n<groupsize,grainsize>(), bulk::there, first, n, result, init, binary_op);
  } // end if
  else
  {
    // Run the parallel raking reduce as an upsweep.
    const int groupsize1 = 128;
    const int grainsize1 = 7;
    typedef mgpu::LaunchBoxVT<groupsize1, grainsize1> Tuning;
    int2 launch = Tuning::GetLaunchParams(context);
    const int NV = launch.x * launch.y;
    
    int numTiles = MGPU_DIV_UP(n, NV);
    int num_blocks = std::min(context.NumSMs() * 25, numTiles);
    int2 task = mgpu::DivideTaskRange(numTiles, num_blocks);
    
    MGPU_MEM(intermediate_type) reductionDevice = context.Malloc<intermediate_type>(num_blocks);
    	
    // n loads + num_blocks stores
    // XXX implement noncommutative_reduce_tiles
    bulk::static_thread_group<groupsize1,grainsize1> group1;
    bulk::async(bulk::par(group1,num_blocks), reduce_tiles<groupsize1,grainsize1>(), bulk::there, first, n, task, reductionDevice->get(), binary_op);
    
    // scan the sums to get the carries
    const int groupsize2 = 256;
    const int grainsize2 = 3;

    // num_blocks loads + num_blocks stores
    bulk::static_thread_group<groupsize2,grainsize2> group2;
    bulk::async(bulk::par(group2,1), exclusive_scan_n<groupsize2,grainsize2>(), bulk::there, reductionDevice->get(), num_blocks, reductionDevice->get(), init, binary_op);
    
    // do the downsweep - n loads, n stores
    bulk::async(bulk::par(group1,num_blocks), inclusive_downsweep<groupsize1,grainsize1>(), bulk::there, first, n, task, reductionDevice->get(), result, binary_op);
  }

  return result + n;
}


template<typename InputIterator, typename T, typename OutputIterator>
OutputIterator my_inclusive_scan(InputIterator first, InputIterator last, T init, OutputIterator result)
{
  mgpu::ContextPtr ctx = mgpu::CreateCudaDevice(0);

  return inclusive_scan(thrust::raw_pointer_cast(&*first),
                        last - first,
                        thrust::raw_pointer_cast(&*result),
                        init,
                        thrust::plus<typename thrust::iterator_value<InputIterator>::type>(),
                        *ctx);
}


typedef int T;


void my_scan(thrust::device_vector<T> *data, T init)
{
  my_inclusive_scan(data->begin(), data->end(), init, data->begin());
}


void do_it(size_t n)
{
  thrust::host_vector<T> h_input(n);
  thrust::fill(h_input.begin(), h_input.end(), 1);

  thrust::host_vector<T> h_result(n);

  T init = 13;

  thrust::inclusive_scan(h_input.begin(), h_input.end(), h_result.begin());
  thrust::for_each(h_result.begin(), h_result.end(), thrust::placeholders::_1 += init);

  thrust::device_vector<T> d_input = h_input;
  thrust::device_vector<T> d_result(d_input.size());

  my_inclusive_scan(d_input.begin(), d_input.end(), init, d_result.begin());

  cudaError_t error = cudaDeviceSynchronize();

  if(error)
  {
    std::cerr << "CUDA error: " << cudaGetErrorString(error) << std::endl;
  }

  assert(h_result == d_result);
}


template<typename InputIterator, typename OutputIterator>
OutputIterator mgpu_inclusive_scan(InputIterator first, InputIterator last, OutputIterator result)
{
  mgpu::ContextPtr ctx = mgpu::CreateCudaDevice(0);

  mgpu::Scan<mgpu::MgpuScanTypeInc>(thrust::raw_pointer_cast(&*first),
                                    last - first,
                                    thrust::raw_pointer_cast(&*result),
                                    mgpu::ScanOp<mgpu::ScanOpTypeAdd,int>(),
                                    (int*)0,
                                    false,
                                    *ctx);

  return result + (last - first);
}


void sean_scan(thrust::device_vector<T> *data)
{
  mgpu_inclusive_scan(data->begin(), data->end(), data->begin());
}


int main()
{
  for(size_t n = 1; n <= 1 << 20; n <<= 1)
  {
    std::cout << "Testing n = " << n << std::endl;
    do_it(n);
  }

  thrust::default_random_engine rng;
  for(int i = 0; i < 20; ++i)
  {
    size_t n = rng() % (1 << 20);
   
    std::cout << "Testing n = " << n << std::endl;
    do_it(n);
  }

  thrust::device_vector<T> vec(1 << 28);

  sean_scan(&vec);
  double sean_msecs = time_invocation_cuda(50, sean_scan, &vec);

  my_scan(&vec, 13);
  double my_msecs = time_invocation_cuda(50, my_scan, &vec, 13);

  std::cout << "Sean's time: " << sean_msecs << " ms" << std::endl;
  std::cout << "My time: " << my_msecs << " ms" << std::endl;

  std::cout << "My relative performance: " << sean_msecs / my_msecs << std::endl;

  return 0;
}

