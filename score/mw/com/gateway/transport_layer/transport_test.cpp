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
#include "score/mw/com/gateway/transport_layer/transport.h"

#include <gtest/gtest.h>

#include <string>

namespace score::mw::com::gateway
{
namespace
{

TEST(DataTypeSizeInfoTest, EqualityComparesAllMembers)
{
    const DataTypeSizeInfo first{8U, 4U};
    const DataTypeSizeInfo same{8U, 4U};
    const DataTypeSizeInfo different_size{16U, 4U};
    const DataTypeSizeInfo different_alignment{8U, 8U};

    EXPECT_EQ(first, same);
    EXPECT_NE(first, different_size);
    EXPECT_NE(first, different_alignment);
}

TEST(ServiceElementConfigurationTest, SerializeMembersExposeMutableReferences)
{
    ServiceElementConfiguration config{"EventA", DataTypeSizeInfo{32U, 8U}};
    auto members = config.GetSerializeMembers();

    std::get<0>(members) = "EventB";
    std::get<1>(members).size = 64U;

    EXPECT_EQ(config.element_name, "EventB");
    EXPECT_EQ(config.size_info.size, 64U);
    EXPECT_EQ(config.size_info.alignment, 8U);
}

}  // namespace
}  // namespace score::mw::com::gateway
