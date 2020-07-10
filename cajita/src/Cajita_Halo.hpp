/****************************************************************************
 * Copyright (c) 2018-2020 by the Cabana authors                            *
 * All rights reserved.                                                     *
 *                                                                          *
 * This file is part of the Cabana library. Cabana is distributed under a   *
 * BSD 3-clause license. For the licensing terms see the LICENSE file in    *
 * the top-level directory.                                                 *
 *                                                                          *
 * SPDX-License-Identifier: BSD-3-Clause                                    *
 ****************************************************************************/

#ifndef CAJITA_HALO_HPP
#define CAJITA_HALO_HPP

#include <Cajita_Array.hpp>
#include <Cajita_IndexSpace.hpp>

#include <Kokkos_Core.hpp>

#include <mpi.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <type_traits>
#include <utility>
#include <vector>

namespace Cajita
{
//---------------------------------------------------------------------------//
// Parameter pack device capture.
//
// NOTE: In general this would not be needed but NVCC cannot capture parameter
// packs in lambda functions hence we need to wrap them in something that can
// be captured.
//---------------------------------------------------------------------------//
// Get the type at the given index of a paremeter pack.
template <std::size_t N, typename T, typename... Types>
struct PackTypeAtIndexImpl;

template <typename T, typename... Types>
struct PackTypeAtIndexImpl<0, T, Types...>
{
    using type = T;
};

template <std::size_t N, typename T, typename... Types>
struct PackTypeAtIndexImpl
{
    using type = typename PackTypeAtIndexImpl<N - 1, Types...>::type;
};

template <std::size_t N, typename... Types>
struct PackTypeAtIndex
{
    using type = typename PackTypeAtIndexImpl<N, Types...>::type;
    static_assert( N < sizeof...( Types ), "Type index out of bounds" );
};

//---------------------------------------------------------------------------//
// Parameter pack element.
template <std::size_t N, typename T>
struct ParameterPackElement
{
    T _m;
};

//---------------------------------------------------------------------------//
// Capture a parameter pack. All parameter pack elements must be copyable to
// device.
template <typename Sequence, typename... Types>
struct ParameterPackImpl;

template <std::size_t... Indices, typename... Types>
struct ParameterPackImpl<std::index_sequence<Indices...>, Types...>
    : ParameterPackElement<Indices, Types>...
{
};

template <typename... Types>
struct ParameterPack
    : ParameterPackImpl<std::make_index_sequence<sizeof...( Types )>, Types...>
{
    template <std::size_t N>
    using value_type = typename PackTypeAtIndex<N, Types...>::type;

    template <std::size_t N>
    using const_value_type = typename std::add_const<value_type<N>>::type;

    template <std::size_t N>
    using element_type = ParameterPackElement<N, value_type<N>>;

    static constexpr std::size_t size = sizeof...( Types );
};

//---------------------------------------------------------------------------//
// Static type checker.
template <class>
struct is_parameter_pack_impl : public std::false_type
{
};

template <typename... Types>
struct is_parameter_pack_impl<ParameterPack<Types...>> : public std::true_type
{
};

template <class T>
struct is_parameter_pack
    : public is_parameter_pack_impl<typename std::remove_cv<T>::type>::type
{
};

//---------------------------------------------------------------------------//
// Get an element from a parameter pack.
template <std::size_t N, class ParameterPack_t>
KOKKOS_FORCEINLINE_FUNCTION typename std::enable_if<
    is_parameter_pack<ParameterPack_t>::value,
    typename ParameterPack_t::template value_type<N> &>::type
get( ParameterPack_t &pp )
{
    return static_cast<typename ParameterPack_t::template element_type<N> &>(
               pp )
        ._m;
}

template <std::size_t N, class ParameterPack_t>
KOKKOS_FORCEINLINE_FUNCTION typename std::enable_if<
    is_parameter_pack<ParameterPack_t>::value,
    const typename ParameterPack_t::template value_type<N> &>::type
get( const ParameterPack_t &pp )
{
    return static_cast<
               const typename ParameterPack_t::template element_type<N> &>( pp )
        ._m;
}

//---------------------------------------------------------------------------//
// Fill a parameter pack. Note the indexing is such that the Nth element of a
// parameter pack is the Nth element of the tuple.
template <typename ParameterPack_t, typename T, typename... Types>
void fillParameterPackImpl( ParameterPack_t &pp,
                            const std::integral_constant<std::size_t, 0>,
                            const T &t, Types... )
{
    get<ParameterPack_t::size - 1>( pp ) = t;
}

template <typename ParameterPack_t, std::size_t N, typename T,
          typename... Types>
void fillParameterPackImpl( ParameterPack_t &pp,
                            const std::integral_constant<std::size_t, N>,
                            const T &t, Types... ts )
{
    get<ParameterPack_t::size - 1 - N>( pp ) = t;
    fillParameterPackImpl( pp, std::integral_constant<std::size_t, N - 1>(),
                           ts... );
}

template <typename ParameterPack_t, typename... Types>
void fillParameterPack( ParameterPack_t &pp, Types... ts )
{
    fillParameterPackImpl(
        pp, std::integral_constant<std::size_t, ParameterPack_t::size - 1>(),
        ts... );
}

//---------------------------------------------------------------------------//
// Create a parameter pack.
template <typename... Types>
ParameterPack<Types...> makeParameterPack( Types... ts )
{
    ParameterPack<Types...> pp;
    fillParameterPack( pp, ts... );
    return pp;
}

//---------------------------------------------------------------------------//
// Halo exchange patterns.
//---------------------------------------------------------------------------//
// Base class.
class HaloPattern
{
  public:
    // Default constructor.
    HaloPattern() {}

    // Destructor
    virtual ~HaloPattern() = default;

    // Assign the neighbors that are in the halo pattern.
    void setNeighbors( const std::vector<std::array<int, 3>> &neighbors )
    {
        _neighbors = neighbors;
    }

    // Get the neighbors that are in the halo pattern.
    std::vector<std::array<int, 3>> getNeighbors() const { return _neighbors; }

  private:
    std::vector<std::array<int, 3>> _neighbors;
};

// Full halo with all 26 adjacent blocks.
class FullHaloPattern : public HaloPattern
{
  public:
    FullHaloPattern()
        : HaloPattern()
    {
        std::vector<std::array<int, 3>> neighbors;
        neighbors.reserve( 26 );
        for ( int i = -1; i < 2; ++i )
            for ( int j = -1; j < 2; ++j )
                for ( int k = -1; k < 2; ++k )
                    if ( !( i == 0 && j == 0 && k == 0 ) )
                        neighbors.push_back( {i, j, k} );
        this->setNeighbors( neighbors );
    }
};

//---------------------------------------------------------------------------//
// Scatter reduction.
//---------------------------------------------------------------------------//
namespace ScatterReduce
{

// Sum values from neighboring ranks into this rank's data.
struct Sum
{
};

// Assign this rank's data to be the minimum of it and its neighbor ranks'
// values.
struct Min
{
};

// Assign this rank's data to be the maximum of it and its neighbor ranks'
// values.
struct Max
{
};

// Replace this rank's data with its neighbor ranks' values. Note that if
// multiple ranks scatter back to the same grid locations then the value
// assigned will be from one of the neighbors but it is undetermined from
// which neighbor that value will come.
struct Replace
{
};

} // end namespace ScatterReduce

//---------------------------------------------------------------------------//
// General multiple array halo communication plan for migrating shared data
// between blocks. Arrays may be defined on different entity types and have
// different data types.
//---------------------------------------------------------------------------//
template <class MemorySpace>
class MultiHalo
{
  public:
    // Memory space.
    using memory_space = MemorySpace;

    /*!
      \brief Constructor.
      \tparam The arrays types to construct the halo for.
      \param pattern The halo pattern to use for halo communication.
      \param width Halo cell width. Must be less than or equal to the halo
      width of the block.
      \param arrays The arrays to build the halo for. These arrays must be
      provided in the same order
    */
    template <class... ArrayTypes>
    MultiHalo( const HaloPattern &pattern, const int width,
               ArrayTypes... arrays )
    {
        // Get the MPI communicator. All arrays should have the same
        // communicator.
        getComm( arrays... );

        // Get the local grid.
        auto local_grid = getLocalGrid( arrays... );

        // Function to get the local id of the neighbor.
        auto neighbor_id = []( const int i, const int j, const int k ) {
            int nk = k + 1;
            int nj = j + 1;
            int ni = i + 1;
            return nk + 3 * ( nj + 3 * ni );
        };

        // Neighbor id flip function. This lets us compute what neighbor we
        // are relative to a given neighbor.
        auto flip = []( const int i ) {
            if ( i == -1 )
                return 1;
            else if ( i == 0 )
                return 0;
            else
                return -1;
        };

        // Get the neighbor ranks we will exchange with in the halo and
        // allocate buffers. If any of the exchanges are self sends mark these
        // so we know which send buffers correspond to which receive buffers.
        auto neighbors = pattern.getNeighbors();
        for ( const auto &n : neighbors )
        {
            // Get the neighbor ids.
            auto i = n[Dim::I];
            auto j = n[Dim::J];
            auto k = n[Dim::K];

            // Get the rank of the neighbor.
            int rank = local_grid->neighborRank( i, j, k );

            // If this is a valid rank add it as a neighbor.
            if ( rank >= 0 )
            {
                // Add the rank.
                _neighbor_ranks.push_back( rank );

                // Set the tag we will use to send data to this neighbor. The
                // receiving rank should have a matching tag.
                _send_tags.push_back( neighbor_id( i, j, k ) );

                // Set the tag we will use to receive data from this
                // neighbor. The sending rank should have a matching tag.
                _receive_tags.push_back(
                    neighbor_id( flip( i ), flip( j ), flip( k ) ) );

                // Create communication data for owned entities.
                buildCommData( Own(), width, i, j, k, _owned_buffers,
                               _owned_steering, arrays... );

                // Create communication data for ghosted entities.
                buildCommData( Ghost(), width, i, j, k, _ghosted_buffers,
                               _ghosted_steering, arrays... );
            }
        }
    }

    // Destructor.
    ~MultiHalo() { MPI_Comm_free( &_comm ); }

    /*!
      \brief Gather data into our ghosts from their owners.

      \param exec_space The execution space to use for pack/unpack.

      \param arrays The arrays to gather. NOTE: These arrays must be given in
      the same order as in the constructor. These could technically be
      different arrays, they just need to have the same layouts and data types
      as the input arrays.
    */
    template <class ExecutionSpace, class... ArrayTypes>
    void gather( const ExecutionSpace &exec_space, ArrayTypes... arrays ) const
    {
        // Get the number of neighbors. Return if we have none.
        int num_n = _neighbor_ranks.size();
        if ( 0 == num_n )
            return;

        // Allocate requests.
        std::vector<MPI_Request> requests( 2 * num_n, MPI_REQUEST_NULL );

        // Pick a tag to use for communication. This object has its own
        // communication space so any tag will do.
        const int mpi_tag = 1234;

        // Post receives.
        for ( int n = 0; n < num_n; ++n )
        {
            // Only process this neighbor if there is work to do.
            if ( 0 < _ghosted_buffers[n].size() )
            {
                MPI_Irecv( _ghosted_buffers[n].data(),
                           _ghosted_buffers[n].size(), MPI_BYTE,
                           _neighbor_ranks[n], mpi_tag + _receive_tags[n],
                           _comm, &requests[n] );
            }
        }

        // Pack send buffers and post sends.
        for ( int n = 0; n < num_n; ++n )
        {
            // Only process this neighbor if there is work to do.
            if ( 0 < _owned_buffers[n].size() )
            {
                // Pack the send buffer.
                packBuffer( exec_space, _owned_buffers[n], _owned_steering[n],
                            arrays.view()... );

                // Post a send.
                MPI_Isend( _owned_buffers[n].data(), _owned_buffers[n].size(),
                           MPI_BYTE, _neighbor_ranks[n],
                           mpi_tag + _send_tags[n], _comm,
                           &requests[num_n + n] );
            }
        }

        // Unpack receive buffers.
        bool unpack_complete = false;
        while ( !unpack_complete )
        {
            // Get the next buffer to unpack.
            int unpack_index = MPI_UNDEFINED;
            MPI_Waitany( num_n, requests.data(), &unpack_index,
                         MPI_STATUS_IGNORE );

            // If there are no more buffers to unpack we are done.
            if ( MPI_UNDEFINED == unpack_index )
            {
                unpack_complete = true;
            }

            // Otherwise unpack the next buffer.
            else
            {
                unpackBuffer( ScatterReduce::Replace(), exec_space,
                              _ghosted_buffers[unpack_index],
                              _ghosted_steering[unpack_index],
                              arrays.view()... );
            }
        }

        // Wait on send requests.
        MPI_Waitall( num_n, requests.data() + num_n, MPI_STATUSES_IGNORE );
    }

    /*!
      \brief Scatter data from our ghosts to their owners using the given type
      of reduce operation.
      \param reduce_op The functor used to reduce the results.
      \param exec_space The execution space to use for pack/unpack.
      \param arrays The arrays to scatter.
    */
    template <class ExecutionSpace, class ReduceOp, class... ArrayTypes>
    void scatter( const ExecutionSpace &exec_space, const ReduceOp &reduce_op,
                  ArrayTypes... arrays ) const
    {
        // Get the number of neighbors. Return if we have none.
        int num_n = _neighbor_ranks.size();
        if ( 0 == num_n )
            return;

        // Requests.
        std::vector<MPI_Request> requests( 2 * num_n, MPI_REQUEST_NULL );

        // Pick a tag to use for communication. This object has its own
        // communication space so any tag will do.
        const int mpi_tag = 2345;

        // Post receives for all neighbors that are not self sends.
        for ( int n = 0; n < num_n; ++n )
        {
            // Only process this neighbor if there is work to do.
            if ( 0 < _owned_buffers[n].size() )
            {
                MPI_Irecv( _owned_buffers[n].data(), _owned_buffers[n].size(),
                           MPI_BYTE, _neighbor_ranks[n],
                           mpi_tag + _receive_tags[n], _comm, &requests[n] );
            }
        }

        // Pack send buffers and post sends.
        for ( int n = 0; n < num_n; ++n )
        {
            // Only process this neighbor if there is work to do.
            if ( 0 < _ghosted_buffers[n].size() )
            {
                // Pack the send buffer.
                packBuffer( exec_space, _ghosted_buffers[n],
                            _ghosted_steering[n], arrays.view()... );

                // Post a send.
                MPI_Isend( _ghosted_buffers[n].data(),
                           _ghosted_buffers[n].size(), MPI_BYTE,
                           _neighbor_ranks[n], mpi_tag + _send_tags[n], _comm,
                           &requests[num_n + n] );
            }
        }

        // Unpack receive buffers.
        bool unpack_complete = false;
        while ( !unpack_complete )
        {
            // Get the next buffer to unpack.
            int unpack_index = MPI_UNDEFINED;
            MPI_Waitany( num_n, requests.data(), &unpack_index,
                         MPI_STATUS_IGNORE );

            // If there are no more buffers to unpack we are done.
            if ( MPI_UNDEFINED == unpack_index )
            {
                unpack_complete = true;
            }

            // Otherwise unpack the next buffer and apply the reduce operation.
            else
            {
                unpackBuffer( reduce_op, exec_space,
                              _owned_buffers[unpack_index],
                              _owned_steering[unpack_index], arrays.view()... );
            }

            // Wait on send requests.
            MPI_Waitall( num_n, requests.data() + num_n, MPI_STATUSES_IGNORE );
        }
    }

  public:
    // Get the byte sizes of the array value types.
    template <class Array_t>
    void getByteSizes( std::vector<int> &byte_sizes, const int array_idx,
                       const Array_t & )
    {
        byte_sizes[array_idx] = sizeof( typename Array_t::value_type );
    }

    template <class Array_t, class... ArrayTypes>
    void getByteSizes( std::vector<int> &byte_sizes, const int array_idx,
                       const Array_t &, ArrayTypes... arrays )
    {
        byte_sizes[array_idx] = sizeof( typename Array_t::value_type );
        getByteSizes( byte_sizes, array_idx + 1, arrays... );
    }

    // Get the communicator.
    template <class Array_t, class... ArrayTypes>
    void getComm( const Array_t &array, ArrayTypes... )
    {
        // Duplicate the communicator so we have our own communication space.
        MPI_Comm_dup( array.layout()->localGrid()->globalGrid().comm(),
                      &_comm );
    }

    // Get the local grid from the arrays.
    template <class Array_t, class... ArrayTypes>
    auto getLocalGrid( const Array_t &array, ArrayTypes... )
    {
        return array.layout()->localGrid();
    }

    // Get the shared index space of an array for a given neighbor in the
    // given decomposition.
    template <class DecompositionTag, class Array_t>
    void getSharedSpace( std::vector<IndexSpace<4>> &shared_spaces,
                         DecompositionTag, const int i, const int j,
                         const int k, const int width, const int array_idx,
                         const Array_t &array )
    {
        shared_spaces[array_idx] = array.layout()->sharedIndexSpace(
            DecompositionTag(), i, j, k, width );
    }

    template <class DecompositionTag, class Array_t, class... ArrayTypes>
    void getSharedSpace( std::vector<IndexSpace<4>> &shared_spaces,
                         DecompositionTag decomposition_tag, const int i,
                         const int j, const int k, const int width,
                         const int array_idx, const Array_t &array,
                         ArrayTypes... arrays )
    {
        shared_spaces[array_idx] = array.layout()->sharedIndexSpace(
            decomposition_tag, i, j, k, width );
        getSharedSpace( shared_spaces, decomposition_tag, i, j, k, width,
                        array_idx + 1, arrays... );
    }

    // Build communication data.
    template <class DecompositionTag, class... ArrayTypes>
    void
    buildCommData( DecompositionTag decomposition_tag, const int width,
                   const int ni, const int nj, const int nk,
                   std::vector<Kokkos::View<char *, memory_space>> &buffers,
                   std::vector<Kokkos::View<int * [6], memory_space>> &steering,
                   ArrayTypes... arrays )
    {
        // Number of arrays.
        int num_array = sizeof...( ArrayTypes );

        // Get the byte sizes of array value types.
        std::vector<int> value_byte_sizes( num_array );
        getByteSizes( value_byte_sizes, 0, arrays... );

        // Get the index spaces we share with this neighbor. We
        // get a shared index space for each array.
        std::vector<IndexSpace<4>> spaces( num_array );
        getSharedSpace( spaces, decomposition_tag, ni, nj, nk, width, 0,
                        arrays... );

        // Compute the buffer size of this neighbor and the
        // number of elements in the buffer.
        int buffer_bytes = 0;
        int buffer_num_element = 0;
        for ( int a = 0; a < num_array; ++a )
        {
            buffer_bytes += value_byte_sizes[a] * spaces[a].size();
            buffer_num_element += spaces[a].size();
        }

        // Allocate the buffer of data that we share with this neighbor. All
        // arrays will be packed into a single buffer.
        buffers.push_back(
            Kokkos::View<char *, memory_space>( "halo_buffer", buffer_bytes ) );

        // Allocate the steering vector for building the buffer.
        steering.push_back( Kokkos::View<int * [6], memory_space>(
            "steering", buffer_num_element ) );

        // Create the steering vector. For each element in the buffer it gives
        // the starting byte location of the element, the array the element is
        // in, and the ijkl structured index in the array of the element.
        auto host_steering =
            Kokkos::create_mirror_view( Kokkos::HostSpace(), steering.back() );
        int elem_counter = 0;
        int byte_counter = 0;
        for ( int a = 0; a < num_array; ++a )
        {
            for ( int i = spaces[a].min( 0 ); i < spaces[a].max( 0 ); ++i )
            {
                for ( int j = spaces[a].min( 1 ); j < spaces[a].max( 1 ); ++j )
                {
                    for ( int k = spaces[a].min( 2 ); k < spaces[a].max( 2 );
                          ++k )
                    {
                        for ( int l = spaces[a].min( 3 );
                              l < spaces[a].max( 3 ); ++l )
                        {
                            // Byte starting location in buffer.
                            host_steering( elem_counter, 0 ) = byte_counter;

                            // Array location of element.
                            host_steering( elem_counter, 1 ) = a;

                            // Structured index in array of element.
                            host_steering( elem_counter, 2 ) = i;
                            host_steering( elem_counter, 3 ) = j;
                            host_steering( elem_counter, 4 ) = k;
                            host_steering( elem_counter, 5 ) = l;

                            // Update element id.
                            ++elem_counter;

                            // Update buffer position.
                            byte_counter += value_byte_sizes[a];
                        }
                    }
                }
            }
        }

        // Check that all elements and bytes are accounted for.
        if ( byte_counter != buffer_bytes )
            throw std::logic_error( "Steering vector contains different number "
                                    "of bytes than buffer" );
        if ( elem_counter != buffer_num_element )
            throw std::logic_error( "Steering vector contains different number "
                                    "of elements than buffer" );

        // Copy steering vector to device.
        Kokkos::deep_copy( steering.back(), host_steering );
    }

    // Pack an element into the buffer. Pack by bytes to avoid casting across
    // alignment boundaries.
    template <class ArrayView>
    KOKKOS_INLINE_FUNCTION void
    packElement( const Kokkos::View<char *, memory_space> &buffer,
                 const Kokkos::View<int * [6], memory_space> &steering,
                 const int element_idx, const ArrayView &array_view ) const
    {
        const char *elem_ptr = reinterpret_cast<const char *>( &array_view(
            steering( element_idx, 2 ), steering( element_idx, 3 ),
            steering( element_idx, 4 ), steering( element_idx, 5 ) ) );
        for ( std::size_t b = 0; b < sizeof( typename ArrayView::value_type );
              ++b )
        {
            buffer( steering( element_idx, 0 ) + b ) = *( elem_ptr + b );
        }
    }

    // Pack an array into a buffer.
    template <class... ArrayViews>
    KOKKOS_INLINE_FUNCTION void
    packArray( const Kokkos::View<char *, memory_space> &buffer,
               const Kokkos::View<int * [6], memory_space> &steering,
               const int element_idx,
               const std::integral_constant<std::size_t, 0>,
               const ParameterPack<ArrayViews...> &array_views ) const
    {
        // If the pack element_idx is in the current array, pack it.
        if ( 0 == steering( element_idx, 1 ) )
            packElement( buffer, steering, element_idx, get<0>( array_views ) );
    }

    // Pack an array into a buffer.
    template <std::size_t N, class... ArrayViews>
    KOKKOS_INLINE_FUNCTION void
    packArray( const Kokkos::View<char *, memory_space> &buffer,
               const Kokkos::View<int * [6], memory_space> &steering,
               const int element_idx,
               const std::integral_constant<std::size_t, N>,
               const ParameterPack<ArrayViews...> &array_views ) const
    {
        // If the pack element_idx is in the current array, pack it.
        if ( N == steering( element_idx, 1 ) )
            packElement( buffer, steering, element_idx, get<N>( array_views ) );

        // Recurse.
        packArray( buffer, steering, element_idx,
                   std::integral_constant<std::size_t, N - 1>(), array_views );
    }

    // Pack arrays into a buffer.
    template <class ExecutionSpace, class... ArrayViews>
    void packBuffer( const ExecutionSpace &exec_space,
                     const Kokkos::View<char *, memory_space> &buffer,
                     const Kokkos::View<int * [6], memory_space> &steering,
                     ArrayViews... array_views ) const
    {
        auto pp = makeParameterPack( array_views... );
        Kokkos::parallel_for(
            "pack_buffer",
            Kokkos::RangePolicy<ExecutionSpace>( exec_space, 0,
                                                 steering.extent( 0 ) ),
            KOKKOS_LAMBDA( const int i ) {
                packArray(
                    buffer, steering, i,
                    std::integral_constant<std::size_t,
                                           sizeof...( ArrayViews ) - 1>(),
                    pp );
            } );
        exec_space.fence();
    }

    // Reduce an element into the buffer. Sum reduction.
    template <class T>
    KOKKOS_INLINE_FUNCTION void
    unpackOp( ScatterReduce::Sum, const T &buffer_val, T &array_val ) const
    {
        array_val += buffer_val;
    }

    // Reduce an element into the buffer. Min reduction.
    template <class T>
    KOKKOS_INLINE_FUNCTION void
    unpackOp( ScatterReduce::Min, const T &buffer_val, T &array_val ) const
    {
        if ( buffer_val < array_val )
            array_val = buffer_val;
    }

    // Reduce an element into the buffer. Max reduction.
    template <class T>
    KOKKOS_INLINE_FUNCTION void
    unpackOp( ScatterReduce::Max, const T &buffer_val, T &array_val ) const
    {
        if ( buffer_val > array_val )
            array_val = buffer_val;
    }

    // Reduce an element into the buffer. Replace reduction.
    template <class T>
    KOKKOS_INLINE_FUNCTION void
    unpackOp( ScatterReduce::Replace, const T &buffer_val, T &array_val ) const
    {
        array_val = buffer_val;
    }

    // Unpack an element from the buffer. Unpack by bytes to avoid casting
    // across alignment boundaries.
    template <class ReduceOp, class ArrayView>
    KOKKOS_INLINE_FUNCTION void
    unpackElement( const ReduceOp &reduce_op,
                   const Kokkos::View<char *, memory_space> &buffer,
                   const Kokkos::View<int * [6], memory_space> &steering,
                   const int element_idx, const ArrayView &array_view ) const
    {
        typename ArrayView::value_type elem;
        char *elem_ptr = reinterpret_cast<char *>( &elem );
        for ( std::size_t b = 0; b < sizeof( typename ArrayView::value_type );
              ++b )
        {
            *( elem_ptr + b ) = buffer( steering( element_idx, 0 ) + b );
        }
        unpackOp( reduce_op, elem,
                  array_view( steering( element_idx, 2 ),
                              steering( element_idx, 3 ),
                              steering( element_idx, 4 ),
                              steering( element_idx, 5 ) ) );
    }

    // Unpack an array from a buffer.
    template <class ReduceOp, class... ArrayViews>
    KOKKOS_INLINE_FUNCTION void
    unpackArray( const ReduceOp &reduce_op,
                 const Kokkos::View<char *, memory_space> &buffer,
                 const Kokkos::View<int * [6], memory_space> &steering,
                 const int element_idx,
                 const std::integral_constant<std::size_t, 0>,
                 const ParameterPack<ArrayViews...> &array_views ) const
    {
        // If the unpack element_idx is in the current array, unpack it.
        if ( 0 == steering( element_idx, 1 ) )
            unpackElement( reduce_op, buffer, steering, element_idx,
                           get<0>( array_views ) );
    }

    // Unpack an array from a buffer.
    template <class ReduceOp, std::size_t N, class... ArrayViews>
    KOKKOS_INLINE_FUNCTION void
    unpackArray( const ReduceOp reduce_op,
                 const Kokkos::View<char *, memory_space> &buffer,
                 const Kokkos::View<int * [6], memory_space> &steering,
                 const int element_idx,
                 const std::integral_constant<std::size_t, N>,
                 const ParameterPack<ArrayViews...> &array_views ) const
    {
        // If the unpack element_idx is in the current array, unpack it.
        if ( N == steering( element_idx, 1 ) )
            unpackElement( reduce_op, buffer, steering, element_idx,
                           get<N>( array_views ) );

        // Recurse.
        unpackArray( reduce_op, buffer, steering, element_idx,
                     std::integral_constant<std::size_t, N - 1>(),
                     array_views );
    }

    // Unpack arrays from a buffer.
    template <class ExecutionSpace, class ReduceOp, class... ArrayViews>
    void unpackBuffer( const ReduceOp &reduce_op,
                       const ExecutionSpace &exec_space,
                       const Kokkos::View<char *, memory_space> &buffer,
                       const Kokkos::View<int * [6], memory_space> &steering,
                       ArrayViews... array_views ) const
    {
        auto pp = makeParameterPack( array_views... );
        Kokkos::parallel_for(
            "unpack_buffer",
            Kokkos::RangePolicy<ExecutionSpace>( exec_space, 0,
                                                 steering.extent( 0 ) ),
            KOKKOS_LAMBDA( const int i ) {
                unpackArray(
                    reduce_op, buffer, steering, i,
                    std::integral_constant<std::size_t,
                                           sizeof...( ArrayViews ) - 1>(),
                    pp );
            } );
    }

  private:
    // MPI communicator.
    MPI_Comm _comm;

    // The ranks we will send/receive from.
    std::vector<int> _neighbor_ranks;

    // The tag we use for sending to each neighbor.
    std::vector<int> _send_tags;

    // The tag we use for receiveing from each neighbor.
    std::vector<int> _receive_tags;

    // For each neighbor, send/receive buffers for data we own.
    std::vector<Kokkos::View<char *, memory_space>> _owned_buffers;

    // For each neighbor, send/receive buffers for data we ghost.
    std::vector<Kokkos::View<char *, memory_space>> _ghosted_buffers;

    // For each neighbor, steering vector for the owned buffer.
    std::vector<Kokkos::View<int * [6], memory_space>> _owned_steering;

    // For each neighbor, steering vector for the ghosted buffer.
    std::vector<Kokkos::View<int * [6], memory_space>> _ghosted_steering;
};

//---------------------------------------------------------------------------//
// Infer array memory space.
template <class ArrayT, class... Types>
struct ArrayPackMemorySpace
{
    using type = typename ArrayT::memory_space;
};

//---------------------------------------------------------------------------//
// Creation function.
template <class... ArrayTypes>
auto createMultiHalo( const HaloPattern &pattern, const int width,
                      ArrayTypes... arrays )
{
    using memory_space = typename ArrayPackMemorySpace<ArrayTypes...>::type;
    return std::make_shared<MultiHalo<memory_space>>( pattern, width,
                                                      arrays... );
}

//---------------------------------------------------------------------------//
// Single array halo exchange communication plan for migrating shared data
// between blocks.
// ---------------------------------------------------------------------------//
template <class Scalar, class DeviceType>
class Halo
{
  public:
    // Scalar type.
    using value_type = Scalar;

    // Device type.
    using device_type = DeviceType;

    // Execution space.
    using execution_space = typename device_type::execution_space;

    // Memory space.
    using memory_space = typename device_type::memory_space;

    // View type.
    using view_type = Kokkos::View<value_type ****, device_type>;

    // Array-like container to hold layout and data information.
    template <class ScalarT, class MemorySpaceT, class ArrayLayoutT>
    struct LayoutContainer
    {
        using value_type = ScalarT;
        using memory_space = MemorySpaceT;
        const ArrayLayoutT &array_layout;
        const ArrayLayoutT *layout() const { return &array_layout; }
    };

    /*!
      \brief Constructor.
      \param layout The array layout to build the halo for.
      \param pattern The halo pattern to use for halo communication
      \param width An option halo cell width. The default is to use the entire
      halo width of the block.
    */
    template <class ArrayLayout_t>
    Halo( const ArrayLayout_t &layout, const HaloPattern &pattern,
          const int width = -1 )
    {
        // Wrap data in an array-like container.
        LayoutContainer<Scalar, memory_space, ArrayLayout_t> container{layout};

        // Create a multi-halo.
        _mhalo = createMultiHalo( pattern, width, container );
    }

    /*!
      \brief Gather data into our ghosts from their owners.
      \param array The array to gather.
    */
    template <class Array_t>
    void gather( const Array_t &array ) const
    {
        _mhalo->gather( execution_space(), array );
    }

    /*!
      \brief Scatter data from our ghosts to their owners. Default sum
      implementation.
      \param array The array to scatter.
    */
    template <class Array_t>
    void scatter( const Array_t &array ) const
    {
        scatter( array, ScatterReduce::Sum() );
    }

    /*!
      \brief Scatter data from our ghosts to their owners using the given type
      of reduce operation.
      \param array The array to scatter.
      \param reduce_op The functor used to reduce the results.
    */
    template <class Array_t, class ReduceOp>
    void scatter( const Array_t &array, const ReduceOp &reduce_op ) const
    {
        _mhalo->scatter( execution_space(), reduce_op, array );
    }

  private:
    std::shared_ptr<MultiHalo<memory_space>> _mhalo;
};

//---------------------------------------------------------------------------//
// Creation function.
//---------------------------------------------------------------------------//
/*!
  \brief Create a halo with a layout.
  \param layout The array layout to build the halo for.
  \note The scalar type and device type must be specified so the proper
  buffers may be allocated. This means a halo constructed via this method is
  only compatible with arrays that have the same scalar and device type.
*/
template <class Scalar, class Device, class EntityType, class MeshType>
std::shared_ptr<Halo<Scalar, Device>>
createHalo( const ArrayLayout<EntityType, MeshType> &layout,
            const HaloPattern &pattern, const int width = -1 )
{
    return std::make_shared<Halo<Scalar, Device>>( layout, pattern, width );
}

//---------------------------------------------------------------------------//
/*!
  \brief Create a halo.
  \param layout The array to build the halo for.
  \note The scalar type and device type are specified via the input arrays so
  the proper buffers may be allocated. This means a halo constructed via this
  method is only compatible with arrays that have the same scalar and device
  type as the input array.
*/
template <class Scalar, class EntityType, class MeshType, class... Params>
std::shared_ptr<
    Halo<typename Array<Scalar, EntityType, MeshType, Params...>::value_type,
         typename Array<Scalar, EntityType, MeshType, Params...>::device_type>>
createHalo( const Array<Scalar, EntityType, MeshType, Params...> &array,
            const HaloPattern &pattern, const int width = -1 )
{
    return std::make_shared<Halo<
        typename Array<Scalar, EntityType, MeshType, Params...>::value_type,
        typename Array<Scalar, EntityType, MeshType, Params...>::device_type>>(
        *( array.layout() ), pattern, width );
}

//---------------------------------------------------------------------------//

} // end namespace Cajita

#endif // end CAJITA_HALO_HPP
