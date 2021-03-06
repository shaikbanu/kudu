// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

package org.apache.kudu.client;

import com.google.protobuf.Message;
import org.jboss.netty.channel.Channel;
import org.jboss.netty.channel.ChannelHandlerContext;
import org.jboss.netty.handler.codec.oneone.OneToOneEncoder;

import org.apache.kudu.rpc.RpcHeader.RequestHeader;

/**
 * An RPC header and associated body protobuf which can be sent outbound
 * through the Netty pipeline. The 'Encoder' inner class is responsible
 * for serializing these instances into wire-format-compatible buffers.
 */
class RpcOutboundMessage {
  private final RequestHeader header;
  private final Message body;

  RpcOutboundMessage(RequestHeader header, Message body) {
    this.header = header;
    this.body = body;
  }

  public RequestHeader getHeader() {
    return header;
  }

  public Message getBody() {
    return body;
  }

  /**
   * Netty encoder implementation to serialize outbound messages.
   */
  static class Encoder extends OneToOneEncoder {
    @Override
    protected Object encode(ChannelHandlerContext ctx, Channel chan,
        Object obj) throws Exception {
      if (!(obj instanceof RpcOutboundMessage)) {
        return obj;
      }
      RpcOutboundMessage msg = (RpcOutboundMessage)obj;
      // TODO(todd): move this impl into this class and remove external
      // callers.
      return KuduRpc.toChannelBuffer(msg.getHeader(), msg.getBody());
    }
  }
}
