/********************************************************************************
 * Copyright (c) 2026 Contributors to the Eclipse Foundation
 *
 * See the NOTICE file(s) distributed with this work for additional
 * information regarding copyright ownership.
 *
 * This program and the accompanying materials are made available under the
 * terms of the Apache License Version 2.0 which is available at
 * https://www.apache.org/licenses/LICENSE-2.0
 *
 * SPDX-License-Identifier: Apache-2.0
 ********************************************************************************/
#include "score/mw/com/gateway/transport_layer/transport_error.h"

#include <gtest/gtest.h>

namespace score::mw::com::gateway
{
namespace
{

class TransportErrorTest : public ::testing::Test
{
  protected:
    void TestErrorMessage(const TransportErrorc error_code, const std::string_view expected_error_output)
    {
        const auto error_code_test =
            transport_error_domain_dummy_.MessageFor(static_cast<score::result::ErrorCode>(error_code));
        ASSERT_EQ(error_code_test, expected_error_output);
    }

    TransportErrorDomain transport_error_domain_dummy_{};
};

TEST_F(TransportErrorTest, MessageForServerSetupFailed)
{
    TestErrorMessage(TransportErrorc::kServerSetupFailed, "Gateway server could not be setup.");
}

TEST_F(TransportErrorTest, MessageForNotSupported)
{
    TestErrorMessage(TransportErrorc::kNotSupported, "Operation not supported by this transport.");
}

TEST_F(TransportErrorTest, MessageForDefault)
{
    TestErrorMessage(static_cast<TransportErrorc>(-1), "unknown transport error");
}

TEST(TransportError, MakeErrorExpectedError)
{
    const score::result::Error err{MakeError(TransportErrorc::kReceiveFailure, "")};
    EXPECT_EQ(*err, static_cast<int>(TransportErrorc::kReceiveFailure));
    EXPECT_TRUE(err.UserMessage().empty());
}

}  // namespace
}  // namespace score::mw::com::gateway
