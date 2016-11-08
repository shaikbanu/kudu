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

import org.apache.kudu.annotations.InterfaceAudience;
import org.apache.kudu.annotations.InterfaceStability;

@InterfaceAudience.Public
@InterfaceStability.Evolving
public class AlterTableResponse extends KuduRpcResponse {

  private String tableId;

  /**
   * @param ellapsedMillis Time in milliseconds since RPC creation to now.
   */
  AlterTableResponse(long ellapsedMillis, String tsUUID, String tableId) {
    super(ellapsedMillis, tsUUID);
    this.tableId = tableId;
  }

  /**
   * @return the ID of the altered table, or null if the master version is too old
   */
  public String getTableId() {
    return tableId;
  }
}