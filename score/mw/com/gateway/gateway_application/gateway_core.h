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
#ifndef SCORE_MW_COM_GATEWAY_GATEWAY_APPLICATION_GATEWAY_CORE_H
#define SCORE_MW_COM_GATEWAY_GATEWAY_APPLICATION_GATEWAY_CORE_H

#include "score/mw/com/gateway/transport_layer/transport.h"
#include "score/mw/com/impl/instance_specifier.h"
#include "score/mw/com/impl/service_element_type.h"
#include "score/result/result.h"

#include <string>
#include <vector>

namespace score::mw::com::gateway
{

class GatewayCore
{
  public:
    virtual ~GatewayCore() = default;

    /// Provide the given service instance locally.
    virtual score::Result<void> ProvideService(impl::InstanceSpecifier service_instance_specifier,
                                               std::vector<ServiceElementConfiguration> service_elements) = 0;

    /// Stop providing the given service instance locally.
    virtual void StopOfferService(impl::InstanceSpecifier service_instance_specifier) = 0;

    /// Offer the given service instance locally.
    virtual score::Result<void> OfferService(impl::InstanceSpecifier service_instance_specifier) = 0;

    /// Register update notification for a local service element.
    virtual score::Result<void> RegisterUpdateNotification(impl::InstanceSpecifier service_instance_specifier,
                                                           impl::ServiceElementType element_type,
                                                           std::string element_name) = 0;

    /// Unregister update notification for a local service element.
    virtual score::Result<void> UnregisterUpdateNotification(impl::InstanceSpecifier service_instance_specifier,
                                                             impl::ServiceElementType element_type,
                                                             std::string element_name) = 0;

    /// Notify an update for the given local service element.
    virtual score::Result<void> NotifyUpdate(impl::InstanceSpecifier service_instance_specifier,
                                             impl::ServiceElementType updated_element_type,
                                             std::string updated_element_name) = 0;
};

}  // namespace score::mw::com::gateway

#endif  // SCORE_MW_COM_GATEWAY_GATEWAY_APPLICATION_GATEWAY_CORE_H
