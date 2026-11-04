/*
 * Copyright (C) 2026 Raspberry Pi Ltd
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

enum class AfterCommand {
    NextCommand,
    ReplyFailThenNext,
    EndSession,
};

// A handler's false means it failed, which is not a reason to drop the host:
// only a transport that can no longer carry replies is. A failed handler that
// sent no OKAY or FAIL still owes the host one, or the host waits forever.
inline AfterCommand DecideAfterCommand(bool handler_ok, bool replied, bool transport_failed) {
    if (transport_failed) return AfterCommand::EndSession;
    if (!handler_ok && !replied) return AfterCommand::ReplyFailThenNext;
    return AfterCommand::NextCommand;
}
