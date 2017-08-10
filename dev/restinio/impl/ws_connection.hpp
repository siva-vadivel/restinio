/*
	restinio
*/

/*!
	WebSocket connection routine.
*/

#pragma once

#include <asio.hpp>

#include <nodejs/http_parser/http_parser.h>

#include <fmt/format.h>

#include <restinio/exception.hpp>
#include <restinio/http_headers.hpp>
#include <restinio/connection_handle.hpp>
#include <restinio/request_handler.hpp>
#include <restinio/impl/header_helpers.hpp>
#include <restinio/impl/response_coordinator.hpp>
#include <restinio/impl/connection_settings.hpp>
#include <restinio/impl/fixed_buffer.hpp>
#include <restinio/impl/raw_resp_output_ctx.hpp>
#include <restinio/impl/websocket_parser.hpp>

namespace restinio
{

namespace impl
{

//
// ws_outgoing_data_t
//

//! A queue for outgoing buffers.
class ws_outgoing_data_t
{
	public:
		//! Add buffers to queue.
		void
		append( buffers_container_t bufs )
		{
			assert( !m_close_when_done );

			if( m_awaiting_buffers.empty() )
			{
				m_awaiting_buffers = std::move( bufs );
			}
			else
			{
				m_awaiting_buffers.reserve( m_awaiting_buffers.size() + bufs.size() );
				for( auto & buf : bufs )
					m_awaiting_buffers.emplace_back( std::move( buf ) );
			}
		}

		void
		pop_ready_buffers(
			std::size_t max_buf_count,
			buffers_container_t & bufs )
		{
			if( max_buf_count >= m_awaiting_buffers.size() )
				bufs = std::move( m_awaiting_buffers );
			else
			{
				const auto begin_of_bunch = m_awaiting_buffers.begin();
				const auto end_of_bunch = begin_of_bunch + max_buf_count;
				bufs.reserve( max_buf_count );
				for( auto it = begin_of_bunch; it != end_of_bunch; ++it )
				{
					bufs.emplace_back( std::move( *it ) );
				}

				m_awaiting_buffers.erase( begin_of_bunch, end_of_bunch );
			}
		}

		bool
		close_when_done() const
		{
			return m_close_when_done;
		}

		void
		set_close_when_done()
		{
			m_close_when_done = true;
		}

	private:
		//! Flag is set when user initiates close.
		/*!
			If flag is switched on, then after sending all the buffers
			the socket mus be closed.
		*/
		bool m_close_when_done{ false };

		//! A queue of buffers.
		buffers_container_t m_awaiting_buffers;
};

//
// ws_connection_t
//

//! Context for handling websocket connections.
/*
*/
template <
		typename TRAITS,
		typename WS_MESSAGE_HANDLER,
		typename WS_CLOSE_HANDLER >
class ws_connection_t final
	:	public ws_connection_base_t
{
	public:
		using message_handler_t = WS_MESSAGE_HANDLER;
		using close_handler_t = WS_CLOSE_HANDLER;
		using logger_t = typename TRAITS::logger_t;
		using strand_t = typename TRAITS::strand_t;
		using stream_socket_t = typename TRAITS::stream_socket_t;

		ws_connection_t(
			//! Connection id.
			std::uint64_t conn_id,
			//! Connection socket.
			stream_socket_t && socket,
			//! Settings that are common for connections.
			connection_settings_shared_ptr_t< TRAITS > settings,
			message_handler_t msg_handler,
			close_handler_t close_handler )
			:	connection_base_t{ conn_id }
			,	m_socket{ std::move( socket ) }
			,	m_strand{ m_socket.get_executor() }
			,	m_settings{ std::move( settings ) }
			,	m_input_header_buffer{ /*TODO: use constant */ 18 }
			,	m_msg_handler{ msg_handler }
			,	m_logger{ *( m_settings->m_logger ) }
		{
			// Notify of a new connection instance.
			m_logger.trace( [&]{
					return fmt::format(
						"[ws_connection:{}] start connection with {}",
						connection_id(),
						m_socket.remote_endpoint() );
			} );
		}

		ws_connection_t( const ws_connection_t & ) = delete;
		ws_connection_t( ws_connection_t && ) = delete;
		void operator = ( const ws_connection_t & ) = delete;
		void operator = ( ws_connection_t && ) = delete;

		~ws_connection_t()
		{
			try
			{
				// Notify of a new connection instance.
				m_logger.trace( [&]{
					return fmt::format(
						"[ws_connection:{}] destroyed",
						connection_id() );
				} );
			}
			catch( ... )
			{}
		}

		virtual void
		close() override
		{
			//! Run write message on io_service loop if possible.
			asio::dispatch(
				get_executor(),
				[ this, ctx = shared_from_this() ](){
					try
					{
						graceful_close();
					}
					catch( const std::exception & ex )
					{
						m_logger.error( [&]{
							return fmt::format(
								"[ws_connection:{}] close operation error: {}",
								connection_id(),
								ex.what() );
						} );
					}
			} );
		}

		//! Start reading ws-messages.
		void
		init_read() override
		{
			//! Run write message on io_service loop if possible.
			asio::dispatch(
				get_executor(),
				[ this, ctx = shared_from_this() ](){
					try
					{
						start_read_header();
					}
					catch( const std::exception & ex )
					{
						trigger_error_and_close(
							ex.what(),
							[&]{
								return fmt::format(
									"[ws_connection:{}] unable to init read: {}",
									connection_id(),
									ex.what() );
							} );
					}
			} );
		}

		//! Write pieces of outgoing data.
		virtual void
		write_data( buffers_container_t bufs ) override
		{
			//! Run write message on io_service loop if possible.
			asio::dispatch(
				get_executor(),
				[ this,
					actual_bufs = std::move( bufs ),
					ctx = shared_from_this() ]() mutable {
						try
						{
							write_data_impl( std::move( actual_bufs ) );
						}
						catch( const std::exception & ex )
						{
							trigger_error_and_close(
								ex.what(),
								[&]{
									return fmt::format(
										"[ws_connection:{}] unable to write data: {}",
										connection_id(),
										ex.what() );
								} );
						}
				} );
		}

	private:
		//! start the process of reading ws messages from socket.
		void
		start_read_header()
		{
			m_logger.trace( [&]{
				 return fmt::format(
						"[ws_connection:{}] start reading header",
						connection_id() );
			} );

			m_socket.async_read_some(
				m_input_header_buffer.make_asio_buffer(),
				asio::bind_executor(
					get_executor(),
					[ this, ctx = shared_from_this() ](
						const asio::error_code & ec,
						std::size_t length ){
							after_read_header( ec, length );
						} ) );
		}

		void
		after_read_header(
			const std::error_code & ec,
			std::size_t length )
		{
			if( !ec )
			{
				// TODO: parse header
				// and

				// if header parsing is complete:
				// m_current_message = new_message( header-smth, payload_length )
				// m_current_message.m_payload
				// if( 0 < m_input_header_buffer.length() )
				// {
				// 	const auto payload_part_size =
				// 		std::min(
				// 			m_input_header_buffer.length(),
				// 			payload_length );

				// 	std::memcpy(
				// 		m_current_message.data(),
				// 		m_input_header_buffer.bytes(),
				// 		payload_part_size );

				// 	m_input_header_buffer.consumed_bytes( payload_part_size );

				// 	if( payload_part_size == payload_length )
				// 	{
				// 		// All message is obtained.
				// 		call_handler_on_current_message();
				// 	}
				// 	else
				// 	{
				// 		// Read the rest of payload:
				// 		start_read_payload(
				// 			m_current_message.data() + payload_part_size,
				// 			payload_length - payload_part_size );
				// 	}
				// }
			}
			else
			{
				// TODO: connection must be closed
				// and user must be notified.
			}
		}

		void
		start_read_payload( const char * payload_data, std::size_t length_remaining )
		{
			m_socket.async_read_some(
				asio::buffer( payload_data, length_remaining ),
				asio::bind_executor(
					get_executor(),
					[ this,
						ctx = shared_from_this(),
						payload_data,
						length_remaining ](
						const asio::error_code & ec,
						std::size_t length ){

							if( ec )
							{
								// TODO: handle error.
							}

							if( length < length_remaining )
							{
								this->start_read_payload(
									payload_data + length,
									length_remaining - length );
							}
							else
							{
								assert( length == length_remaining );

								// All message is obtained.
								// call_handler_on_current_message();
							}
						} ) );
		}

		void
		write_data_impl( buffers_container_t bufs )
		{
			assert( m_socket );

			if( !m_socket.is_open() )
			{
				m_logger.warn( [&]{
					return fmt::format(
							"[ws_connection:{}] try to write response, "
							"while socket is closed",
							connection_id() );
				} );
				return;
			}
			else if( m_awaiting_buffers.close_when_done() )
			{
				// User closed ws-connection before.

				//TODO: it might be the case to leave only an assert here
				// because there should be no way to init write
				// after websocket_t object is closed.
				// Depends on whether it is considered to be used in parallel
				// wnen `close()` call and `message_send()` call
				// can happen in parallel threads.

				m_logger.warn( [&]{
					return fmt::format(
							"[ws_connection:{}] try to write response "
							"after sebsocket was closed",
							connection_id() );
				} );
				return;
			}

			m_awaiting_buffers.append( std::move( bufs ) );

			init_write_if_necessary();
		}

		// Check if there is something to write,
		// and if so starts write operation.
		void
		init_write_if_necessary()
		{
			if( !m_resp_out_ctx.transmitting() )
			{
				if( m_resp_out_ctx.obtain_bufs( m_awaiting_buffers ) )
				{
					auto & bufs = m_resp_out_ctx.create_bufs();

					m_logger.trace( [&]{
						return fmt::format(
							"[ws_connection:{}] sending resp data, "
							"buf count: {}",
							connection_id(),
							bufs.size() ); } );

					// There is somethig to write.
					asio::async_write(
						m_socket,
						bufs,
						asio::bind_executor(
							get_executor(),
							[ this,
								ctx = shared_from_this() ]
								( const asio::error_code & ec, std::size_t written ){
									try
									{
										this->after_write(
											ec,
											written );
									}
									catch( const std::exception & ex )
									{
										trigger_error_and_close(
											ex.what(),
											[&]{
												return fmt::format(
													"[ws_connection:{}] after write callback error: {}",
													connection_id(),
													ex.what() );
											} );
									}
							} ) );

					// TODO: guard_write_operation();
				}
				else if ( m_awaiting_buffers.close_when_done() )
				{
					call_close_handler( "user initiated" );
					close_impl();
				}
			}
		}

		//! Handle write response finished.
		inline void
		after_write(
			const std::error_code & ec,
			std::size_t written )
		{
			if( !ec )
			{
				// Release buffers.
				m_resp_out_ctx.done();

				m_logger.trace( [&]{
					return fmt::format(
							"[ws_connection:{}] outgoing data was sent: {}b",
							connection_id() );
				} );

				if( m_socket.is_open() )
				{
					// Start another write opertion
					// if there is something to send.
					init_write_if_necessary();
				}
			}
			else
			{
				if( ec != asio::error::operation_aborted )
				{
					trigger_error_and_close(
						ec.message(),
						[&]{
							return fmt::format(
								"[ws_connection:{}] unable to write: {}",
								connection_id(),
								ec.message() );
						} );
				}
				// else: Operation aborted only in case of close was called.
			}
		}

		//! Close WebSocket connection in a graceful manner
		//! sending a close-message
		void
		graceful_close()
		{
			if( !m_awaiting_buffers.close_when_done() )
			{
				// TODO:
				// Send close frame.
				// m_awaiting_buffers.append( ??? );
				m_awaiting_buffers.set_close_when_done();
				init_write_if_necessary();
			}
		}


		//! An executor for callbacks on async operations.
		inline strand_t &
		get_executor()
		{
			return m_strand;
		}

		//! Close connection functions.
		//! \{

		//! Standard close routine.
		void
		close_impl()
		{
			m_logger.trace( [&]{
				return fmt::format(
						"[ws_connection:{}] close",
						connection_id() );
			} );

			asio::error_code ignored_ec;
			m_socket.shutdown(
				asio::ip::tcp::socket::shutdown_both,
				ignored_ec );
			m_socket.close();
		}

		//! Trigger an error.
		/*!
			Closes the connection and write to log
			an error message.
		*/
		template< typename MSG_BUILDER >
		void
		trigger_error_and_close(
			const std::string & reason,
			MSG_BUILDER && msg_builder )
		{
			m_logger.error( std::move( msg_builder ) );
			call_close_handler( reason );
		}
		//! \}

		void
		call_close_handler( const std::string & reason )
		{
			if( m_close_handler )
			{
				m_close_handler( reason );
				m_close_handler = close_handler_t{};
			}
		}

		//! Connection.
		stream_socket_t m_socket;

		//! Sync object for connection events.
		strand_t m_strand;

		//! Common paramaters of a connection.
		connection_settings_shared_ptr_t< TRAITS > m_settings;

		message_handler_t m_msg_handler;
		close_handler_t m_close_handler;

		//! Input routine.
		fixed_buffer_t m_input_header_buffer;

		//! Write to socket operation context.
		raw_resp_output_ctx_t m_resp_out_ctx;

		//! Output buffers queue.
		ws_outgoing_data_t m_awaiting_buffers;

		//! Logger for operation
		logger_t & m_logger;
};

} /* namespace impl */

} /* namespace restinio */
