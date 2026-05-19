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
#ifndef SCORE_MW_COM_GATEWAY_TRANSPORT_LAYER_TRANSPORT_H
#define SCORE_MW_COM_GATEWAY_TRANSPORT_LAYER_TRANSPORT_H

#include "score/mw/com/impl/instance_specifier.h"
#include "score/mw/com/impl/service_element_type.h"
#include "score/result/result.h"

#include <cstddef>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

namespace score::mw::com::gateway
{

struct DataTypeSizeInfo
{
    std::size_t size{0U};
    std::size_t alignment{0U};

    friend bool operator==(const DataTypeSizeInfo& lhs, const DataTypeSizeInfo& rhs) noexcept
    {
        return lhs.size == rhs.size && lhs.alignment == rhs.alignment;
    }

    friend bool operator!=(const DataTypeSizeInfo& lhs, const DataTypeSizeInfo& rhs) noexcept
    {
        return !(lhs == rhs);
    }
};

static_assert(std::is_trivially_copyable_v<DataTypeSizeInfo>, "DataTypeSizeInfo must be trivially copyable");

struct ServiceElementConfiguration
{
    std::string element_name;
    DataTypeSizeInfo size_info;

    std::tuple<const std::string&, const DataTypeSizeInfo&> GetSerializeMembers() const
    {
        return {element_name, size_info};
    }

    std::tuple<std::string&, DataTypeSizeInfo&> GetSerializeMembers()
    {
        return {element_name, size_info};
    }
};

class Transport
{
  public:
    Transport() = default;
    virtual ~Transport() = default;

    virtual bool IsMemorySharingSupported() = 0;

    virtual score::Result<void> Setup() = 0;
    virtual void Shutdown() = 0;

    virtual score::Result<void> ProvideService(impl::InstanceSpecifier service_instance_specifier,
                                               std::vector<ServiceElementConfiguration> service_elements) = 0;

    virtual score::Result<void> OfferService(impl::InstanceSpecifier service_instance_specifier) = 0;

    virtual score::Result<void> StopOfferService(impl::InstanceSpecifier service_instance_specifier) = 0;

    virtual score::Result<void> NotifyUpdate(impl::InstanceSpecifier service_instance_specifier,
                                             impl::ServiceElementType updated_element_type,
                                             std::string updated_element_name) = 0;

    virtual score::Result<void> RegisterUpdateNotification(impl::InstanceSpecifier service_instance_specifier,
                                                           impl::ServiceElementType element_type,
                                                           std::string element_name) = 0;

    virtual score::Result<void> UnregisterUpdateNotification(impl::InstanceSpecifier service_instance_specifier,
                                                             impl::ServiceElementType element_type,
                                                             std::string element_name) = 0;
};

}  // namespace score::mw::com::gateway

#endif  // SCORE_MW_COM_GATEWAY_TRANSPORT_LAYER_TRANSPORT_H
