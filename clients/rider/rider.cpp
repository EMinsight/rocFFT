
#include <iostream>
#include <sstream>
#include <functional>
#include <cmath>
#include <valarray>

#include <boost/program_options.hpp>
#include "rocfft.h"
#include "../../src/include/unicode.compatibility.h"
#include "./rider.h"

#define __HIPCC__
#include <hip_runtime.h>

#include "./misc.h"

namespace po = boost::program_options;

//	This is used with the program_options class so that the user can type an integer on the command line
//	and we store into an enum varaible
template<class _Elem, class _Traits>
std::basic_istream<_Elem, _Traits> & operator>> (std::basic_istream<_Elem, _Traits> & stream, rocfft_array_type & atype)
{
	unsigned tmp;
	stream >> tmp;
	atype = rocfft_array_type(tmp);
	return stream;
}

// similarly for transform type
template<class _Elem, class _Traits>
std::basic_istream<_Elem, _Traits> & operator>> (std::basic_istream<_Elem, _Traits> & stream, rocfft_transform_type & ttype)
{
	unsigned tmp;
	stream >> tmp;
	ttype = rocfft_transform_type(tmp);
	return stream;
}

template < typename T >
int transform( size_t* lengths, const size_t *inStrides, const size_t *outStrides, size_t batchSize,
		rocfft_array_type inArrType, rocfft_array_type outArrType,
		rocfft_result_placement place, rocfft_precision precision, rocfft_transform_type transformType,
		int deviceId, int platformId, bool printInfo,
		unsigned profile_count
		)
{
	//	Our command line does not specify what dimension FFT we wish to transform; we decode
	//	this from the lengths that the user specifies for X, Y, Z.  A length of one means that
	//	The user does not want that dimension.

	const size_t max_dimensions = 3;
	size_t strides[ 4 ];
	size_t o_strides[ 4 ];
	size_t fftVectorSize = 0;
	size_t fftVectorSizePadded = 0;
	size_t fftBatchSize = 0;
	size_t outfftVectorSize = 0;
	size_t outfftVectorSizePadded = 0;
	size_t outfftBatchSize = 0;
	size_t size_of_input_buffers_in_bytes = 0;
	size_t size_of_output_buffers_in_bytes = 0;
	unsigned number_of_output_buffers = 0;
	unsigned dim = 1;
	void *input_device_buffers [2] = { NULL, NULL };
	void *output_device_buffers[2] = { NULL, NULL };
	std::vector< int > device_id;

	if( lengths[ 1 ] > 1 )
	{
		dim	= 2;
	}
	if( lengths[ 2 ] > 1 )
	{
		dim	= 3;
	}

	strides[ 0 ] = inStrides[0];
	strides[ 1 ] = inStrides[1];
	strides[ 2 ] = inStrides[2];
	strides[ 3 ] = inStrides[3];

	o_strides[ 0 ] = outStrides[0];
	o_strides[ 1 ] = outStrides[1];
	o_strides[ 2 ] = outStrides[2];
	o_strides[ 3 ] = outStrides[3];

	fftVectorSize = lengths[0] * lengths[1] * lengths[2];
	fftVectorSizePadded = strides[3];
	fftBatchSize = fftVectorSizePadded * batchSize;

	size_t Nt = 1 + lengths[0]/2;

	if(place == rocfft_placement_inplace)
	{
		outfftVectorSize = fftVectorSize;
		outfftVectorSizePadded = fftVectorSizePadded;
		outfftBatchSize = fftBatchSize;
	}
	else
	{
		outfftVectorSize = lengths[0] * lengths[1] * lengths[2];
		outfftVectorSizePadded = o_strides[3];
		outfftBatchSize = outfftVectorSizePadded * batchSize;
	}


	// Real to complex case
	if( (inArrType == rocfft_array_type_real) || (outArrType == rocfft_array_type_real) )
	{
		fftVectorSizePadded = strides[3];
		fftBatchSize = fftVectorSizePadded * batchSize;

		outfftVectorSizePadded = o_strides[3];
		outfftBatchSize = outfftVectorSizePadded * batchSize;

		fftVectorSize = lengths[0] * lengths[1] * lengths[2];
		outfftVectorSize = fftVectorSize;

	}


	switch( outArrType )
	{
	case rocfft_array_type_complex_interleaved:
		number_of_output_buffers = 1;
		size_of_output_buffers_in_bytes = outfftBatchSize * sizeof( std::complex< T > );
		break;
	case rocfft_array_type_complex_planar:
		number_of_output_buffers = 2;
		size_of_output_buffers_in_bytes = outfftBatchSize * sizeof(T);
		break;
	case rocfft_array_type_hermitian_interleaved:
		number_of_output_buffers = 1;
		size_of_output_buffers_in_bytes = outfftBatchSize * sizeof( std::complex< T > );
		break;
	case rocfft_array_type_hermitian_planar:
		number_of_output_buffers = 2;
		size_of_output_buffers_in_bytes = outfftBatchSize * sizeof(T);
		break;
	case rocfft_array_type_real:
		number_of_output_buffers = 1;
		size_of_output_buffers_in_bytes = outfftBatchSize * sizeof(T);
		break;
	}

	// Fill the input buffers
	switch( inArrType )
	{
	case rocfft_array_type_complex_interleaved:
		{
			//	This call creates our openCL context and sets up our devices; expected to throw on error
			size_of_input_buffers_in_bytes = fftBatchSize * sizeof( std::complex< T > );

			setupBuffers( 
				device_id,
				size_of_input_buffers_in_bytes, 1, input_device_buffers,
				size_of_output_buffers_in_bytes, number_of_output_buffers, output_device_buffers);

			std::vector< std::complex< T > > input( fftBatchSize );

			// set zero
			for( unsigned i = 0; i < fftBatchSize; ++i )
			{
				input[ i ] = 0;
			}

			// impulse test case
			for(size_t b = 0; b < batchSize; b++)
			{
				size_t p3 = b * strides[3];
				for(size_t k = 0; k < lengths[2]; k++)
				{
					size_t p2 = p3 + k * strides[2];
					for(size_t j = 0; j < lengths[1]; j++)
					{
						size_t p1 = p2 + j * strides[1];
						for(size_t i = 0; i < lengths[0]; i++)
						{
							size_t p0 = p1 + i * strides[0];
							input[p0] = 1;
						}
					}
				}
			}


			HIP_V_THROW( hipMemcpy( input_device_buffers[ 0 ], &input[ 0 ], size_of_input_buffers_in_bytes, hipMemcpyHostToDevice), "hipMemcpy failed" );

		}
		break;
	case rocfft_array_type_complex_planar:
		{
			//	This call creates our openCL context and sets up our devices; expected to throw on error
			size_of_input_buffers_in_bytes = fftBatchSize * sizeof( T );

			setupBuffers( 
				device_id,
				size_of_input_buffers_in_bytes, 2, input_device_buffers,
				size_of_output_buffers_in_bytes, number_of_output_buffers, output_device_buffers);

			std::vector< T > real( fftBatchSize );
			std::vector< T > imag( fftBatchSize );

			// set zero
			for( unsigned i = 0; i < fftBatchSize; ++i )
			{
				real[ i ] = 0;
				imag[ i ] = 0;
			}

			// impulse test case
			for(size_t b = 0; b < batchSize; b++)
			{
				size_t p3 = b * strides[3];
				for(size_t k = 0; k < lengths[2]; k++)
				{
					size_t p2 = p3 + k * strides[2];
					for(size_t j = 0; j < lengths[1]; j++)
					{
						size_t p1 = p2 + j * strides[1];
						for(size_t i = 0; i < lengths[0]; i++)
						{
							size_t p0 = p1 + i * strides[0];
							real[p0] = 1;
						}
					}
				}
			}


			HIP_V_THROW( hipMemcpy( input_device_buffers[ 0 ], &real[ 0 ], size_of_input_buffers_in_bytes, hipMemcpyHostToDevice), "hipMemcpy failed" );
			HIP_V_THROW( hipMemcpy( input_device_buffers[ 1 ], &imag[ 0 ], size_of_input_buffers_in_bytes, hipMemcpyHostToDevice), "hipMemcpy failed" );
		}
		break;
	case rocfft_array_type_hermitian_interleaved:
		{
			//	This call creates our openCL context and sets up our devices; expected to throw on error
			size_of_input_buffers_in_bytes = fftBatchSize * sizeof( std::complex< T > );

			setupBuffers( 
				device_id,
				size_of_input_buffers_in_bytes, 1, input_device_buffers,
				size_of_output_buffers_in_bytes, number_of_output_buffers, output_device_buffers);

			std::vector< std::complex< T > > input( fftBatchSize );

			// set zero
			for( unsigned i = 0; i < fftBatchSize; ++i )
			{
				input[ i ] = 0;
			}

			// impulse test case
			for(size_t b = 0; b < batchSize; b++)
			{
				size_t p3 = b * strides[3];
				input[p3] = static_cast<T>(outfftVectorSize);

			}


			HIP_V_THROW( hipMemcpy( input_device_buffers[ 0 ], &input[ 0 ], size_of_input_buffers_in_bytes, hipMemcpyHostToDevice), "hipMemcpy failed" );
		}
		break;
	case rocfft_array_type_hermitian_planar:
		{
			//	This call creates our openCL context and sets up our devices; expected to throw on error
			size_of_input_buffers_in_bytes = fftBatchSize * sizeof( T );

			setupBuffers( 
				device_id,
				size_of_input_buffers_in_bytes, 2, input_device_buffers,
				size_of_output_buffers_in_bytes, number_of_output_buffers, output_device_buffers);

			std::vector< T > real( fftBatchSize );
			std::vector< T > imag( fftBatchSize );

			// set zero
			for( unsigned i = 0; i < fftBatchSize; ++i )
			{
				real[ i ] = 0;
				imag[ i ] = 0;
			}

			// impulse test case
			for(size_t b = 0; b < batchSize; b++)
			{
				size_t p3 = b * strides[3];
				real[p3] = static_cast<T>(outfftVectorSize);
			}



			HIP_V_THROW( hipMemcpy( input_device_buffers[ 0 ], &real[ 0 ], size_of_input_buffers_in_bytes, hipMemcpyHostToDevice), "hipMemcpy failed" );
			HIP_V_THROW( hipMemcpy( input_device_buffers[ 1 ], &imag[ 0 ], size_of_input_buffers_in_bytes, hipMemcpyHostToDevice), "hipMemcpy failed" );
		}
		break;
	case rocfft_array_type_real:
		{
			//	This call creates our openCL context and sets up our devices; expected to throw on error
			size_of_input_buffers_in_bytes = fftBatchSize * sizeof( T );

			setupBuffers( 
				device_id,
				size_of_input_buffers_in_bytes, 1, input_device_buffers,
				size_of_output_buffers_in_bytes, number_of_output_buffers, output_device_buffers);

			std::vector< T > real( fftBatchSize );

			// set zero
			for( unsigned i = 0; i < fftBatchSize; ++i )
			{
				real[ i ] = 0;
			}

			// impulse test case
			for(size_t b = 0; b < batchSize; b++)
			{
				size_t p3 = b * strides[3];
				for(size_t k = 0; k < lengths[2]; k++)
				{
					size_t p2 = p3 + k * strides[2];
					for(size_t j = 0; j < lengths[1]; j++)
					{
						size_t p1 = p2 + j * strides[1];
						for(size_t i = 0; i < lengths[0]; i++)
						{
							size_t p0 = p1 + i * strides[0];
							real[p0] = 1;
						}
					}
				}
			}


			HIP_V_THROW( hipMemcpy( input_device_buffers[ 0 ], &real[ 0 ], size_of_input_buffers_in_bytes, hipMemcpyHostToDevice), "hipMemcpy failed" );
		}
		break;
	default:
		{
			throw std::runtime_error( "Input layout format not yet supported" );
		}
		break;
	}

#if 0
	HIP_V_THROW( clfftSetup( setupData.get( ) ), "clfftSetup failed" );
	HIP_V_THROW( clfftCreateDefaultPlan( &plan_handle, context, dim, lengths ), "clfftCreateDefaultPlan failed" );

	//	Default plan creates a plan that expects an inPlace transform with interleaved complex numbers
	HIP_V_THROW( clfftSetResultLocation( plan_handle, place ), "clfftSetResultLocation failed" );
	HIP_V_THROW( clfftSetLayout( plan_handle, inArrType, outArrType ), "clfftSetLayout failed" );
	HIP_V_THROW( clfftSetPlanBatchSize( plan_handle, batchSize ), "clfftSetPlanBatchSize failed" );
	HIP_V_THROW( clfftSetPlanPrecision( plan_handle, precision ), "clfftSetPlanPrecision failed" );

	HIP_V_THROW (clfftSetPlanInStride  ( plan_handle, dim, strides ), "clfftSetPlanInStride failed" );
	HIP_V_THROW (clfftSetPlanOutStride ( plan_handle, dim, o_strides ), "clfftSetPlanOutStride failed" );
	HIP_V_THROW (clfftSetPlanDistance  ( plan_handle, strides[ 3 ], o_strides[ 3 ]), "clfftSetPlanDistance failed" );

	// Set backward scale factor to 1.0 for non real FFTs to do correct output checks
	if(dir == CLFFT_BACKWARD && inArrType != rocfft_array_type_real && outArrType != rocfft_array_type_real)
		HIP_V_THROW (clfftSetPlanScale( plan_handle, CLFFT_BACKWARD, (cl_float)1.0f ), "clfftSetPlanScale failed" );

	HIP_V_THROW( clfftBakePlan( plan_handle, 1, &queue, NULL, NULL ), "clfftBakePlan failed" );

	//get the buffersize
	size_t buffersize=0;
	HIP_V_THROW( clfftGetTmpBufSize(plan_handle, &buffersize ), "clfftGetTmpBufSize failed" );

	//allocate the intermediate buffer
	void *clMedBuffer=NULL;

	if (buffersize)
	{
		int medstatus;
		clMedBuffer = clCreateBuffer ( context, CL_MEM_READ_WRITE, buffersize, 0, &medstatus);
		HIP_V_THROW( medstatus, "Creating intmediate Buffer failed" );
	}
#endif

	switch( inArrType )
	{
	case rocfft_array_type_complex_interleaved:
	case rocfft_array_type_complex_planar:
	case rocfft_array_type_hermitian_interleaved:
	case rocfft_array_type_hermitian_planar:
	case rocfft_array_type_real:
		break;
	default:
		//	Don't recognize input layout
		return rocfft_status_invalid_arg_value;
	}

	switch( outArrType )
	{
	case rocfft_array_type_complex_interleaved:
	case rocfft_array_type_complex_planar:
	case rocfft_array_type_hermitian_interleaved:
	case rocfft_array_type_hermitian_planar:
	case rocfft_array_type_real:
		break;
	default:
		//	Don't recognize output layout
		return rocfft_status_invalid_arg_value;
	}

	if (( place == rocfft_placement_inplace )
	&&  ( inArrType != outArrType )) {
		switch( inArrType )
		{
		case rocfft_array_type_complex_interleaved:
			{
				if( (outArrType == rocfft_array_type_complex_planar) || (outArrType == rocfft_array_type_hermitian_planar) )
				{
					throw std::runtime_error( "Cannot use the same buffer for interleaved->planar in-place transforms" );
				}
				break;
			}
		case rocfft_array_type_complex_planar:
			{
				if( (outArrType == rocfft_array_type_complex_interleaved) || (outArrType == rocfft_array_type_hermitian_interleaved) )
				{
					throw std::runtime_error( "Cannot use the same buffer for planar->interleaved in-place transforms" );
				}
				break;
			}
		case rocfft_array_type_hermitian_interleaved:
			{
				if( outArrType != rocfft_array_type_real )
				{
					throw std::runtime_error( "Cannot use the same buffer for interleaved->planar in-place transforms" );
				}
				break;
			}
		case rocfft_array_type_hermitian_planar:
			{
				throw std::runtime_error( "Cannot use the same buffer for planar->interleaved in-place transforms" );
				break;
			}
		case rocfft_array_type_real:
			{
				if( (outArrType == rocfft_array_type_complex_planar) || (outArrType == rocfft_array_type_hermitian_planar) )
				{
					throw std::runtime_error( "Cannot use the same buffer for interleaved->planar in-place transforms" );
				}
				break;
			}
		}
	}

	void **BuffersOut = ( place == rocfft_placement_inplace ) ? NULL : &output_device_buffers[ 0 ];

#if 0
	// Execute once for basic functional test
	HIP_V_THROW( clfftEnqueueTransform( plan_handle, dir, 1, &queue, 0, NULL, NULL,
		&input_device_buffers[ 0 ], BuffersOut, clMedBuffer ),
		"clfftEnqueueTransform failed" );

	HIP_V_THROW( clFinish( queue ), "clFinish failed" );
	

	cl_event *outEvent = new cl_event[profile_count];
	for( unsigned i = 0; i < profile_count; ++i ) outEvent[i] = 0;

	if(profile_count > 1)
	{
		Timer tr;		
		tr.Start();
		for( unsigned i = 0; i < profile_count; ++i )
		{
			if( timer ) timer->Start( clFFTID );

			HIP_V_THROW( clfftEnqueueTransform( plan_handle, dir, 1, &queue, 0, NULL, &outEvent[i],
				&input_device_buffers[ 0 ], BuffersOut, clMedBuffer ),
				"clfftEnqueueTransform failed" );

			if( timer ) timer->Stop( clFFTID );
		}
		HIP_V_THROW( clWaitForEvents ( profile_count, outEvent ), "clWaitForEvents  failed" );

		double wtime = tr.Sample()/((double)profile_count);

		HIP_V_THROW( clFinish( queue ), "clFinish failed" );

		size_t totalLen = 1;
		for(int i=0; i<dim; i++) totalLen *= lengths[i];

		double constMultiplier = 1.0;
		if( (inArrType == rocfft_array_type_real ) || (outArrType == rocfft_array_type_real) )
			constMultiplier = 2.5;
		else
			constMultiplier = 5.0;

		double opsconst = constMultiplier * (double)totalLen * log((double)totalLen) / log(2.0);


		tout << "\nExecution wall time: " << 1000.0*wtime << " ms" << std::endl;
		tout << "Execution gflops: " << ((double)batchSize * opsconst)/(1000000000.0*wtime) << std::endl;

	}

	if(clMedBuffer) clReleaseMemObject(clMedBuffer);

	if( timer && (command_queue_flags & CL_QUEUE_PROFILING_ENABLE) )
	{
		//	Remove all timings that are outside of 2 stddev (keep 65% of samples); we ignore outliers to get a more consistent result
		timer->pruneOutliers( 2.0 );
		timer->Print( );
		timer->Reset( );
	}

	/*****************/
	FreeSharedLibrary( timerLibHandle );

	for( unsigned i = 0; i < profile_count; ++i )
	{
		if(outEvent[i])
			clReleaseEvent(outEvent[i]);
	}

	delete[] outEvent;
#endif
	// Read and check output data
	// This check is not valid if the FFT is executed multiple times inplace.
	//
	if (( place == rocfft_placement_notinplace )
	||  ( profile_count == 1))
	{
		bool checkflag= false;
		switch( outArrType )
		{
		case rocfft_array_type_hermitian_interleaved:
		case rocfft_array_type_complex_interleaved:
			{
				std::vector< std::complex< T > > output( outfftBatchSize );

				if( place == rocfft_placement_inplace )
				{
					HIP_V_THROW( hipMemcpy( &output[ 0 ], input_device_buffers[ 0 ], size_of_input_buffers_in_bytes, hipMemcpyDeviceToHost), "hipMemcpy failed" );
				}
				else
				{
					HIP_V_THROW( hipMemcpy( &output[ 0 ], BuffersOut[ 0 ], size_of_output_buffers_in_bytes, hipMemcpyDeviceToHost), "hipMemcpy failed" );
				}

				//check output data
				for( unsigned i = 0; i < outfftBatchSize; ++i )
				{
					if (0 == (i % outfftVectorSizePadded))
					{
						if (output[i].real() != outfftVectorSize)
						{
							checkflag = true;
							break;
						}
					}
					else
					{
						if (output[ i ].real() != 0)
						{
							checkflag = true;
							break;
						}
					}

					if (output[ i ].imag() != 0)
					{
						checkflag = true;
						break;
					}
				}
			}
			break;
		case rocfft_array_type_hermitian_planar:
		case rocfft_array_type_complex_planar:
			{
				std::valarray< T > real( outfftBatchSize );
				std::valarray< T > imag( outfftBatchSize );

				if( place == rocfft_placement_inplace )
				{
					HIP_V_THROW( hipMemcpy( &real[ 0 ], input_device_buffers[ 0 ], size_of_input_buffers_in_bytes, hipMemcpyDeviceToHost), "hipMemcpy failed" );
					HIP_V_THROW( hipMemcpy( &imag[ 0 ], input_device_buffers[ 1 ], size_of_input_buffers_in_bytes, hipMemcpyDeviceToHost), "hipMemcpy failed" );
				}
				else
				{
					HIP_V_THROW( hipMemcpy( &real[ 0 ], BuffersOut[ 0 ], size_of_output_buffers_in_bytes, hipMemcpyDeviceToHost), "hipMemcpy failed" );
					HIP_V_THROW( hipMemcpy( &imag[ 0 ], BuffersOut[ 1 ], size_of_output_buffers_in_bytes, hipMemcpyDeviceToHost), "hipMemcpy failed" );
				}

				//  Check output data
				for( unsigned i = 0; i < outfftBatchSize; ++i )
				{
					if (0 == (i % outfftVectorSizePadded))
					{
						if (real[i] != outfftVectorSize)
						{
							checkflag = true;
							break;
						}
					}
					else
					{
						if (real[i] != 0)
						{
							checkflag = true;
							break;
						}
					}

					if (imag[i] != 0)
					{
						checkflag = true;
						break;
					}
				}
			}
			break;
		case rocfft_array_type_real:
			{
				std::valarray< T > real( outfftBatchSize );

				if( place == rocfft_placement_inplace )
				{
					HIP_V_THROW( hipMemcpy( &real[ 0 ], input_device_buffers[ 0 ], size_of_input_buffers_in_bytes, hipMemcpyDeviceToHost), "hipMemcpy failed" );
				}
				else
				{
					HIP_V_THROW( hipMemcpy( &real[ 0 ], BuffersOut[ 0 ], size_of_output_buffers_in_bytes, hipMemcpyDeviceToHost), "hipMemcpy failed" );
				}

				////check output data

				for(size_t b = 0; b < batchSize; b++)
				{
					size_t p3 = b * o_strides[3];
					for(size_t k = 0; k < lengths[2]; k++)
					{
						size_t p2 = p3 + k * o_strides[2];
						for(size_t j = 0; j < lengths[1]; j++)
						{
							size_t p1 = p2 + j * o_strides[1];
							for(size_t i = 0; i < lengths[0]; i++)
							{
								size_t p0 = p1 + i * o_strides[0];

								if (real[p0] != 1)
								{
									checkflag = true;
									break;
								}

							}
						}
					}
				}
			}
			break;
		default:
			{
				throw std::runtime_error( "Input layout format not yet supported" );
			}
			break;
		}

		if (checkflag)
		{
			std::cout << "\n\n\t\tInternal Client Test *****FAIL*****" << std::endl;
		}
		else
		{
			std::cout << "\n\n\t\tInternal Client Test *****PASS*****" << std::endl;
		}
	}

	// HIP_V_THROW( clfftDestroyPlan( &plan_handle ), "clfftDestroyPlan failed" );
	// HIP_V_THROW( clfftTeardown( ), "clfftTeardown failed" );

	clearBuffers( countOf( input_device_buffers ), input_device_buffers, countOf( output_device_buffers ), output_device_buffers );
	return 0;
}


int _tmain( int argc, _TCHAR* argv[] )
{
	//	This helps with mixing output of both wide and narrow characters to the screen
	std::ios::sync_with_stdio( false );

	int				deviceId = 0;
	int				platformId = 0;

	//	FFT state

	rocfft_result_placement place = rocfft_placement_inplace;
	rocfft_transform_type	transformType = rocfft_transform_type_complex_forward;
	rocfft_array_type	inArrType  = rocfft_array_type_complex_interleaved;
	rocfft_array_type	outArrType = rocfft_array_type_complex_interleaved;
	rocfft_precision precision = rocfft_precision_single;

	size_t lengths[ 3 ] = {1,1,1};
	size_t iStrides[ 4 ] = {0,0,0,0};
	size_t oStrides[ 4 ] = {0,0,0,0};
	unsigned profile_count = 0;

	unsigned command_queue_flags = 0;
	size_t batchSize = 1;

	try
	{
		// Declare the supported options.
		po::options_description desc( "rocfft rider command line options" );
		desc.add_options()
			( "help,h",        "produces this help message" )
			( "version,v",     "Print queryable version information from the rocfft library" )
			( "info,i",      "Print queryable information of all the runtimes and devices" )
			( "printChosen",   "Print queryable information of the selected runtime and device" )
			( "platform",      po::value< int >( &platformId )->default_value( 0 ),   "Select a specific platform id as it is reported by info" )
			( "device",        po::value< int >( &deviceId )->default_value( 0 ),   "Select a specific device id as it is reported by info" )
			( "notInPlace,o",    "Not in-place FFT transform (default: in-place)" )
			( "double",		   "Double precision transform (default: single)" )
			( "transformType,t",	po::value< rocfft_transform_type >( &transformType )->default_value( rocfft_transform_type_complex_forward ), "Type of transform:\n0) complex forward\n1) complex inverse\n2) real forward\n3) real inverse" )
			( "lenX,x",        po::value< size_t >( &lengths[ 0 ] )->default_value( 1024 ),   "Specify the length of the 1st dimension of a test array" )
			( "lenY,y",        po::value< size_t >( &lengths[ 1 ] )->default_value( 1 ),      "Specify the length of the 2nd dimension of a test array" )
			( "lenZ,z",        po::value< size_t >( &lengths[ 2 ] )->default_value( 1 ),      "Specify the length of the 3rd dimension of a test array" )
			( "isX",   po::value< size_t >( &iStrides[ 0 ] )->default_value( 1 ),		"Specify the input stride of the 1st dimension of a test array" )
			( "isY",   po::value< size_t >( &iStrides[ 1 ] )->default_value( 0 ),	"Specify the input stride of the 2nd dimension of a test array" )
			( "isZ",   po::value< size_t >( &iStrides[ 2 ] )->default_value( 0 ),	"Specify the input stride of the 3rd dimension of a test array" )
			( "iD", po::value< size_t >( &iStrides[ 3 ] )->default_value( 0 ), "input distance between successive members when batch size > 1" )
			( "osX",   po::value< size_t >( &oStrides[ 0 ] )->default_value( 1 ),		"Specify the output stride of the 1st dimension of a test array" )
			( "osY",   po::value< size_t >( &oStrides[ 1 ] )->default_value( 0 ),	"Specify the output stride of the 2nd dimension of a test array" )
			( "osZ",   po::value< size_t >( &oStrides[ 2 ] )->default_value( 0 ),	"Specify the output stride of the 3rd dimension of a test array" )
			( "oD", po::value< size_t >( &oStrides[ 3 ] )->default_value( 0 ), "output distance between successive members when batch size > 1" )
			( "batchSize,b",   po::value< size_t >( &batchSize )->default_value( 1 ), "If this value is greater than one, arrays will be used " )
			( "profile,p",     po::value< unsigned >( &profile_count )->default_value( 1 ), "Time and report the kernel speed of the FFT (default: profiling off)" )
			( "inArrType",      po::value< rocfft_array_type >( &inArrType )->default_value( rocfft_array_type_complex_interleaved ), "Array type of input data:\n0) interleaved\n1) planar\n2) hermitian interleaved\n3) hermitian planar\n4) real" )
			( "outArrType",     po::value< rocfft_array_type >( &outArrType )->default_value( rocfft_array_type_complex_interleaved ), "Array type of output data:\n0) interleaved\n1) planar\n2) hermitian interleaved\n3) hermitian planar\n4) real" )
			;

		po::variables_map vm;
		po::store( po::parse_command_line( argc, argv, desc ), vm );
		po::notify( vm );

		if( vm.count( "version" ) )
		{
			std::cout << "version" << std::endl;
			return 0;
		}

		if( vm.count( "help" ) )
		{
			//	This needs to be 'cout' as program-options does not support wcout yet
			std::cout << desc << std::endl;
			return 0;
		}

		
		if( vm.count( "info" ) )
		{
			return 0;
		}

		bool printInfo = false;
		if( vm.count( "printChosen" ) )
		{
			printInfo = true;
		}

		if( vm.count( "notInPlace" ) )
		{
			place = rocfft_placement_notinplace;
		}

		if( vm.count( "double" ) )
		{
			precision = rocfft_precision_double;
		}


		if( profile_count > 1 )
		{
		}

		int inL = (int)inArrType;
		int otL = (int)outArrType;

		// input output array type support matrix
		int ioArrTypeSupport[5][5] =		{
										{ 1, 1, 0, 0, 1 },
										{ 1, 1, 0, 0, 1 },
										{ 0, 0, 0, 0, 1 },
										{ 0, 0, 0, 0, 1 },
										{ 1, 1, 1, 1, 0 },
										};

		if(inL > 4) throw std::runtime_error( "Invalid Input array type format" );
		if(otL > 4) throw std::runtime_error( "Invalid Output array type format" );

		if(ioArrTypeSupport[inL][otL] == 0) throw std::runtime_error( "Invalid combination of Input/Output array type formats" );

		if( (transformType == rocfft_transform_type_complex_forward) || (transformType == rocfft_transform_type_complex_inverse) ) // Complex-Complex cases
		{
			iStrides[1] = iStrides[1] ? iStrides[1] : lengths[0] * iStrides[0];
			iStrides[2] = iStrides[2] ? iStrides[2] : lengths[1] * iStrides[1];
			iStrides[3] = iStrides[3] ? iStrides[3] : lengths[2] * iStrides[2];

			if(place == rocfft_placement_inplace)
			{
				oStrides[0] = iStrides[0];
				oStrides[1] = iStrides[1];
				oStrides[2] = iStrides[2];
				oStrides[3] = iStrides[3];
			}
			else
			{
				oStrides[1] = oStrides[1] ? oStrides[1] : lengths[0] * oStrides[0];
				oStrides[2] = oStrides[2] ? oStrides[2] : lengths[1] * oStrides[1];
				oStrides[3] = oStrides[3] ? oStrides[3] : lengths[2] * oStrides[2];
			}
		}
		else // Real cases
		{
			size_t *rst, *cst;
			size_t N = lengths[0];
			size_t Nt = 1 + lengths[0]/2;
			bool iflag = false;
			bool rcFull = (inL == 0) || (inL == 1) || (otL == 0) || (otL == 1);

			if(inArrType == rocfft_array_type_real ) { iflag = true; rst = iStrides; }
			else { rst = oStrides; } // either in or out should be REAL

			// Set either in or out strides whichever is real
			if(place == rocfft_placement_inplace)
			{
				if(rcFull)	{ rst[1] = rst[1] ? rst[1] :  N * 2 * rst[0]; }
				else		{ rst[1] = rst[1] ? rst[1] : Nt * 2 * rst[0]; }

				rst[2] = rst[2] ? rst[2] : lengths[1] * rst[1];
				rst[3] = rst[3] ? rst[3] : lengths[2] * rst[2];
			}
			else
			{
				rst[1] = rst[1] ? rst[1] : lengths[0] * rst[0];
				rst[2] = rst[2] ? rst[2] : lengths[1] * rst[1];
				rst[3] = rst[3] ? rst[3] : lengths[2] * rst[2];
			}

			// Set the remaining of in or out strides that is not real
			if(iflag) { cst = oStrides; }
			else	  { cst = iStrides; }

			if(rcFull)	{ cst[1] = cst[1] ? cst[1] :  N * cst[0]; }
			else		{ cst[1] = cst[1] ? cst[1] : Nt * cst[0]; }

			cst[2] = cst[2] ? cst[2] : lengths[1] * cst[1];
			cst[3] = cst[3] ? cst[3] : lengths[2] * cst[2];
		}
		
		if( precision == rocfft_precision_single )
			transform<float>( lengths, iStrides, oStrides, batchSize, inArrType, outArrType, place, precision, transformType, deviceId, platformId, printInfo, profile_count );
		else
			transform<double>( lengths, iStrides, oStrides, batchSize, inArrType, outArrType, place, precision, transformType, deviceId, platformId, printInfo, profile_count ); 
	}
	catch( std::exception& e )
	{
		terr << _T( "rocfft error condition reported:" ) << std::endl << e.what() << std::endl;
		return 1;
	}

	return 0;
}
