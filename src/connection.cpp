// Copyright (c) 2018, ZIH,
// Technische Universitaet Dresden,
// Federal Republic of Germany
//
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without modification,
// are permitted provided that the following conditions are met:
//
//     * Redistributions of source code must retain the above copyright notice,
//       this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above copyright notice,
//       this list of conditions and the following disclaimer in the documentation
//       and/or other materials provided with the distribution.
//     * Neither the name of metricq nor the names of its contributors
//       may be used to endorse or promote products derived from this software
//       without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
// LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
// NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#include <metricq/connection.hpp>

#include <metricq/logger.hpp>

#include "log.hpp"
#include "util.hpp"

#include <amqpcpp.h>
extern "C"
{
#include <openssl/ssl.h>
}

#include <nlohmann/json.hpp>

#include <iostream>
#include <memory>
#include <string>

namespace metricq
{
static std::string make_token(const std::string& token, bool add_uuid)
{
    if (add_uuid)
    {
        return uuid(token);
    }
    return token;
}

Connection::Connection(const std::string& connection_token, bool add_uuid,
                       std::size_t concurrency_hint)
: io_service(concurrency_hint), connection_token_(make_token(connection_token, add_uuid)),
  management_handler_(io_service)
{
}

Connection::~Connection()
{
}

void Connection::main_loop()
{
    io_service.run();
}

static void init_ssl()
{
    // THIS IS NOT THREADSAFE
    static bool is_initialized = false;
    if (is_initialized)
    {
        return;
    }

    // init the SSL library (this works for openssl 1.1, for openssl 1.0 use SSL_library_init())
    // OPENSSL_init_ssl(0, NULL);
    SSL_library_init();

    is_initialized = true;
}

void Connection::connect(const std::string& server_address)
{
    if (server_address.substr(0, 5) == "amqps")
    {
        init_ssl();
    }

    management_address_ = server_address;

    log::info("connecting to management server: {}", *management_address_);

    management_connection_ =
        std::make_unique<AMQP::TcpConnection>(&management_handler_, *management_address_);
    management_channel_ = std::make_unique<AMQP::TcpChannel>(management_connection_.get());
    management_channel_->onReady(debug_success_cb("management channel ready"));
    management_channel_->onError(debug_error_cb("management channel error"));

    management_client_queue_ = std::string("client-") + connection_token_ + "-rpc";

    management_channel_->declareQueue(management_client_queue_, AMQP::exclusive)
        .onSuccess([this](const std::string& name, [[maybe_unused]] int msgcount,
                          [[maybe_unused]] int consumercount) {
            management_channel_
                ->bindQueue(management_broadcast_exchange_, management_client_queue_, "#")
                .onError(debug_error_cb("error binding management queue to broadcast exchange"))
                .onSuccess([this, name]() {
                    management_channel_->consume(name)
                        .onReceived([this](const AMQP::Message& message, uint64_t delivery_tag,
                                           bool redelivered) {
                            handle_management_message(message, delivery_tag, redelivered);
                        })
                        .onSuccess([this]() { on_connected(); })
                        .onError(debug_error_cb("management consume error"));
                });
        });
}

void Connection::register_management_callback(const std::string& function, ManagementCallback cb)
{
    auto ret = management_callbacks_.emplace(function, std::move(cb));
    if (!ret.second)
    {
        log::error("trying to register management callback that is already registered: {}", function);
        throw std::invalid_argument("trying to register management callback that is already registered");
    }
}

void Connection::rpc(const std::string& function, ManagementResponseCallback response_callback,
                     json payload)
{
    log::debug("management rpc sending {}", function);
    payload["function"] = function;
    std::string message = payload.dump();
    AMQP::Envelope envelope(message.data(), message.size());

    auto correlation_id = uuid(std::string("metricq-rpc-") + connection_token_ + "-");
    envelope.setCorrelationID(correlation_id);
    envelope.setAppID(connection_token_);
    assert(!management_client_queue_.empty());
    envelope.setReplyTo(management_client_queue_);
    envelope.setContentType("application/json");
    envelope.setPersistent(true);

    auto ret =
        management_rpc_response_callbacks_.emplace(correlation_id, std::move(response_callback));
    if (!ret.second)
    {
        log::error("trying to register management RPC response callback that is already registered: {}", correlation_id);
        throw std::invalid_argument("trying to register management RPC response callback that is already registered");
    }

    management_channel_->publish(management_exchange_, function, envelope);
}

void Connection::handle_management_message(const AMQP::Message& incoming_message,
                                           uint64_t deliveryTag, [[maybe_unused]] bool redelivered)
{
    const std::string content_str(incoming_message.body(),
                                  static_cast<size_t>(incoming_message.bodySize()));
    try
    {
        log::debug("management rpc response received: {}", content_str);

        auto content = json::parse(content_str);

        if (auto it = management_rpc_response_callbacks_.find(incoming_message.correlationID());
            it != management_rpc_response_callbacks_.end())
        {
            // Incoming message is a RPC-response, call the response handler
            if (content.count("error"))
            {
                log::error("management rpc failed: {}. stopping",
                           content["error"].get<std::string>());
                stop();
            }
            else
            {
                // This may be debatable... but right now we don't want to bother the caller with
                // error handling. We may need it in the future.
                it->second(content);
            }
            management_rpc_response_callbacks_.erase(it);
        }
        else if (auto it = management_callbacks_.find(content["function"]);
                 it != management_callbacks_.end())
        {
            log::debug("management rpc call received: {}", content_str);
            // incoming message is a RPC-call
            auto response = it->second(content);

            std::string reply_message = response.dump();
            AMQP::Envelope envelope(reply_message.data(), reply_message.size());
            envelope.setCorrelationID(incoming_message.correlationID());
            envelope.setAppID(connection_token_);
            envelope.setContentType("application/json");

            log::debug("sending reply '{}' to {} / {}", reply_message, incoming_message.replyTo(),
                       incoming_message.correlationID());
            envelope.setPersistent(true);
            management_channel_->publish("", incoming_message.replyTo(), envelope);
        }
        else
        {
            log::warn("message not found as rpc response or callback");
        }
    }
    catch (nlohmann::json::parse_error& e)
    {
        log::error("error in rpc response: parsing message: {}", e.what());
    }
    catch (nlohmann::json::type_error& e)
    {
        log::error("error in rpc response: accessing parameter: {}", e.what());
    }

    management_channel_->ack(deliveryTag);
}

AMQP::Address Connection::add_credentials(const AMQP::Address& address)
{
    return AMQP::Address(address.hostname(), address.port(), management_address_->login(),
                         address.vhost(), address.secure());
}

void Connection::close()
{
    if (!management_connection_)
    {
        log::debug("closing connection, no management_connection up yet");
        return;
    }
    auto alive = management_connection_->close();
    log::info("closed management_connection: {}", alive);
}

void Connection::stop()
{
    log::debug("requesting stop");
    close();
    log::info("stopping io_service");
    // the io_service will stop itself once all connections are closed
}
} // namespace metricq
