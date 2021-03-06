.. _config_dubbo_filters_router:

Router
======

The router filter implements Dubbo forwarding. It will be used in almost all Dubbo proxying
scenarios. The filter's main job is to follow the instructions specified in the configured
:ref:`route table <envoy_api_msg_config.filter.network.dubbo_proxy.v2alpha1.RouteConfiguration>`.

* :ref:`v2 API reference <envoy_api_msg_config.filter.dubbo.router.v2alpha1.Router>`
* This filter should be configured with the name *envoy.filters.dubbo.router*.
