// Copyright (c) 2018, ZIH, Technische Universitaet Dresden, Federal Republic of Germany
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

#include "log.hpp"

#include <metricq/json.hpp>
#include <metricq/transformer.hpp>

namespace metricq
{
Transformer::Transformer(const std::string& token) : Sink(token)
{
}

void Transformer::on_connected()
{
    rpc("transformer.register", [this](const auto& response) { config(response); });
}

void Transformer::send(const std::string& id, const DataChunk& dc)
{
    data_channel_->publish(data_exchange_, id, dc.SerializeAsString());
}

void Transformer::send(const std::string& id, TimeValue tv)
{
    // TODO evaluate optimization of string construction
    data_channel_->publish(data_exchange_, id, DataChunk(tv).SerializeAsString());
}

void Transformer::config(const json& config)
{
    if (!data_exchange_.empty() && config["dataExchange"] != data_exchange_)
    {
        log::fatal("changing dataExchange on the fly is not currently supported");
        std::abort();
    }

    data_exchange_ = config["dataExchange"];

    if (config.find("config") != config.end())
    {
        on_transformer_config(config["config"]);
    }

    if (input_metrics.empty())
    {
        log::fatal("required input metrics not set");
        std::abort();
    }
    rpc("transformer.subscribe",
        [this](const json& response) {
            if (this->data_queue_.empty())
            {
                this->sink_config(response);
            }
            if (this->data_queue_ != response.at("dataQueue"))
            {
                throw std::runtime_error("inconsistent sink dataQueue setting after subscription");
            }

            on_transformer_ready();
            declare_metrics();
        },
        { { "metrics", input_metrics } });
}

void Transformer::declare_metrics()
{
    if (output_metrics_.empty())
    {
        return;
    }

    json payload;
    for (auto& metric : output_metrics_)
    {
        payload["metrics"][metric.second.id()] = metric.second.metadata.json();
    }
    rpc(
        "transformer.declare_metrics",
        [this](const auto&) { /* nothing to do */
                              (void)this;
        },
        payload);
}
} // namespace metricq
