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

#include <catch2/catch_test_macros.hpp>

#include "session_policy.h"

TEST_CASE("A command that failed and said so keeps the session", "[session]") {
    // bmap-verify on a range past the end of the eMMC: FAIL, then false.
    CHECK(DecideAfterCommand(false, true, false) == AfterCommand::NextCommand);
}

TEST_CASE("A command that failed silently is answered for it", "[session]") {
    CHECK(DecideAfterCommand(false, false, false) == AfterCommand::ReplyFailThenNext);
}

TEST_CASE("A command that succeeded moves on", "[session]") {
    CHECK(DecideAfterCommand(true, true, false) == AfterCommand::NextCommand);
    // A handler that succeeded owns its reply; the loop never adds one.
    CHECK(DecideAfterCommand(true, false, false) == AfterCommand::NextCommand);
}

TEST_CASE("A failed transport ends the session whatever the handler said", "[session]") {
    CHECK(DecideAfterCommand(true, true, true) == AfterCommand::EndSession);
    CHECK(DecideAfterCommand(false, true, true) == AfterCommand::EndSession);
    CHECK(DecideAfterCommand(false, false, true) == AfterCommand::EndSession);
}
