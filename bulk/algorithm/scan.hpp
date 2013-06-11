#pragma once

#include <bulk/execution_group.hpp>
#include <bulk/malloc.hpp>
#include <thrust/reduce.h>
#include <thrust/scan.h>
#include <thrust/execution_policy.h>
#include <thrust/detail/type_traits.h>
#include <thrust/detail/type_traits/function_traits.h>
#include <thrust/detail/type_traits/iterator/is_output_iterator.h>
#include <thrust/system/cuda/detail/detail/uninitialized.h>

namespace bulk
{
namespace detail
{
namespace scan_detail
{


template<typename InputIterator, typename OutputIterator, typename BinaryFunction>
struct scan_intermediate
  : thrust::detail::eval_if<
      thrust::detail::has_result_type<BinaryFunction>::value,
      thrust::detail::result_type<BinaryFunction>,
      thrust::detail::eval_if<
        thrust::detail::is_output_iterator<OutputIterator>::value,
        thrust::iterator_value<InputIterator>,
        thrust::iterator_value<OutputIterator>
      >
    >
{};


template<unsigned int size, typename ExecutionGroup, typename T, typename BinaryFunction>
__device__ T small_inplace_exclusive_scan_with_buffer(ExecutionGroup &g, T *first, T init, T *buffer, BinaryFunction binary_op)
{
  // XXX int is noticeably faster than ExecutionGroup::size_type
  typedef int size_type;
  //typedef typename ExecutionGroup::size_type size_type;

  // ping points to the most current data
  T *ping = first;
  T *pong = buffer;

  size_type tid = g.this_exec.index();

  if(tid == 0)
  {
    first[0] = binary_op(init, first[0]);
  }

  T x = first[tid];

  g.wait();

  for(size_type offset = 1; offset < size; offset += offset)
  {
    if(tid >= offset)
    {
      x = binary_op(ping[tid - offset], x);
    }

    thrust::swap(ping, pong);

    ping[tid] = x;

    g.wait();
  }

  T result = ping[size - 1];

  x = (tid == 0) ? init : ping[tid - 1];

  g.wait();

  first[tid] = x;

  g.wait();

  return result;
} // end small_inplace_exclusive_scan_with_buffer()


template<std::size_t groupsize, std::size_t grainsize, typename RandomAccessIterator1, typename RandomAccessIterator2, typename BinaryFunction>
struct scan_buffer
{
  typedef typename thrust::iterator_value<RandomAccessIterator1>::type  input_type;

  typedef typename scan_intermediate<
    RandomAccessIterator1,
    RandomAccessIterator2,
    BinaryFunction
  >::type intermediate_type;

  intermediate_type sums[2*groupsize];

  union
  {
    input_type        inputs[groupsize * grainsize];
    intermediate_type results[groupsize * grainsize];
  };
};


template<bool inclusive, std::size_t groupsize, std::size_t grainsize, typename RandomAccessIterator1, typename RandomAccessIterator2, typename T, typename BinaryFunction>
__device__ void scan_with_buffer(bulk::static_execution_group<groupsize,grainsize> &g,
                                 RandomAccessIterator1 first, RandomAccessIterator1 last,
                                 RandomAccessIterator2 result,
                                 T carry_in,
                                 BinaryFunction binary_op,
                                 scan_buffer<groupsize,grainsize,RandomAccessIterator1,RandomAccessIterator2,BinaryFunction> &buffer)
{
  typedef scan_buffer<
    groupsize,
    grainsize,
    RandomAccessIterator1,
    RandomAccessIterator2,
    BinaryFunction
  > buffer_type;

  typedef typename buffer_type::input_type        input_type;
  typedef typename buffer_type::intermediate_type intermediate_type;

  // XXX grabbing this pointer up front before the loop is noticeably
  //     faster than dereferencing inputs or results inside buffer
  //     in the loop below
  union {
    input_type        *inputs;
    intermediate_type *results;
  } stage;

  stage.inputs = buffer.inputs;

  // XXX int is noticeably faster than ExecutionGroup::size_type
  //typedef typename bulk::static_execution_group<groupsize,grainsize>::size_type size_type;
  typedef int size_type;

  size_type tid = g.this_exec.index();

  size_type elements_per_group = groupsize * grainsize;

  for(; first < last; first += elements_per_group, result += elements_per_group)
  {
    size_type partition_size = thrust::min<size_type>(elements_per_group, last - first);
    
    // stage data through shared memory
    bulk::copy_n(g, first, partition_size, stage.inputs);
    
    // Transpose out of shared memory.
    input_type local_inputs[grainsize];

    size_type local_offset = grainsize * tid;

    size_type local_size = thrust::max<size_type>(0,thrust::min<size_type>(grainsize, partition_size - grainsize * tid));

    // XXX this should be uninitialized<input_type>
    input_type x;

    // this loop is a fused copy and accumulate
    #pragma unroll
    for(size_type i = 0; i < grainsize; ++i)
    {
      size_type index = local_offset + i;
      if(index < partition_size)
      {
        local_inputs[i] = stage.inputs[index];
        x = i ? binary_op(x, local_inputs[i]) : local_inputs[i];
      } // end if
    } // end for

    if(local_size)
    {
      buffer.sums[tid] = x;
    } // end if

    g.wait();
    
    // exclusive scan the array of per-thread sums
    carry_in = small_inplace_exclusive_scan_with_buffer<groupsize>(g, buffer.sums, carry_in, buffer.sums + groupsize, binary_op);

    if(local_size)
    {
      x = buffer.sums[tid];
    } // end if
    
    // this loop is a scan (x begins as the init)
    #pragma unroll
    for(size_type i = 0; i < grainsize; ++i) 
    {
      size_type index = local_offset + i;
      if(index < partition_size)
      {
        if(inclusive)
        {
          x = binary_op(x, local_inputs[i]);
        } // end if

        stage.results[index] = x;

        if(!inclusive)
        {
          x = binary_op(x, local_inputs[i]);
        } // end if
      } // end if
    } // end for

    g.wait();
    
    bulk::copy_n(g, stage.results, partition_size, result);
  } // end for
} // end scan_with_buffer()


} // end scan_detail
} // end detail


template<std::size_t groupsize,
         std::size_t grainsize,
         typename RandomAccessIterator1,
         typename RandomAccessIterator2,
         typename T,
         typename BinaryFunction>
__device__ void inclusive_scan(bulk::static_execution_group<groupsize,grainsize> &g,
                               RandomAccessIterator1 first, RandomAccessIterator1 last,
                               RandomAccessIterator2 result,
                               T init,
                               BinaryFunction binary_op)
{
  typedef detail::scan_detail::scan_buffer<groupsize,grainsize,RandomAccessIterator1,RandomAccessIterator2,BinaryFunction> buffer_type;

#if __CUDA_ARCH__ >= 200
  buffer_type *buffer = reinterpret_cast<buffer_type*>(bulk::malloc(g, sizeof(buffer_type)));

  if(bulk::detail::is_shared(buffer))
  {
    detail::scan_detail::scan_with_buffer<true>(g, first, last, result, init, binary_op, *bulk::detail::on_chip_cast(buffer));
  } // end if
  else
  {
    detail::scan_detail::scan_with_buffer<true>(g, first, last, result, init, binary_op, *buffer);
  } // end else

  bulk::free(g, buffer);
#else
  __shared__ thrust::system::cuda::detail::detail::uninitialized<buffer_type> buffer;
  detail::scan_detail::scan_with_buffer<true>(g, first, last, result, init, binary_op, *bulk::detail::on_chip_cast(&buffer.get()));
#endif // __CUDA_ARCH__
} // end inclusive_scan()


template<std::size_t size,
         std::size_t grainsize,
         typename RandomAccessIterator1,
         typename RandomAccessIterator2,
         typename BinaryFunction>
__device__
RandomAccessIterator2 inclusive_scan(static_execution_group<size,grainsize> &this_group,
                                     RandomAccessIterator1 first,
                                     RandomAccessIterator1 last,
                                     RandomAccessIterator2 result,
                                     BinaryFunction binary_op)
{
  if(first < last)
  {
    // the first input becomes the init
    typename thrust::iterator_value<RandomAccessIterator1>::type init = *first;

    if(this_group.this_exec.index() == 0)
    {
      *result = init;
    } // end if

    bulk::inclusive_scan(this_group, first + 1, last, result + 1, init, binary_op);
  } // end if

  return result + (last - first);
} // end inclusive_scan()


template<std::size_t groupsize,
         std::size_t grainsize,
         typename RandomAccessIterator1,
         typename RandomAccessIterator2,
         typename T,
         typename BinaryFunction>
__device__ void exclusive_scan(bulk::static_execution_group<groupsize,grainsize> &g,
                               RandomAccessIterator1 first, RandomAccessIterator1 last,
                               RandomAccessIterator2 result,
                               T init,
                               BinaryFunction binary_op)
{
  typedef detail::scan_detail::scan_buffer<groupsize,grainsize,RandomAccessIterator1,RandomAccessIterator2,BinaryFunction> buffer_type;

#if __CUDA_ARCH__ >= 200
  buffer_type *buffer = reinterpret_cast<buffer_type*>(bulk::malloc(g, sizeof(buffer_type)));

  if(bulk::detail::is_shared(buffer))
  {
    detail::scan_detail::scan_with_buffer<false>(g, first, last, result, init, binary_op, *bulk::detail::on_chip_cast(buffer));
  } // end if
  else
  {
    detail::scan_detail::scan_with_buffer<false>(g, first, last, result, init, binary_op, *buffer);
  } // end else

  bulk::free(g, buffer);
#else
  __shared__ thrust::system::cuda::detail::detail::uninitialized<buffer_type> buffer;
  detail::scan_detail::scan_with_buffer<false>(g, first, last, result, init, binary_op, buffer.get());
#endif
} // end exclusive_scan()


} // end bulk

