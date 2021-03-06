#include "envoy/config/cluster/v3/cluster.pb.h"
#include "envoy/extensions/clusters/aggregate/v3/cluster.pb.h"
#include "envoy/extensions/clusters/aggregate/v3/cluster.pb.validate.h"

#include "common/singleton/manager_impl.h"

#include "extensions/clusters/aggregate/cluster.h"

#include "test/common/upstream/utility.h"
#include "test/mocks/protobuf/mocks.h"
#include "test/mocks/server/mocks.h"
#include "test/mocks/ssl/mocks.h"
#include "test/test_common/environment.h"

using testing::Eq;
using testing::Return;
using testing::ReturnRef;

namespace Envoy {
namespace Extensions {
namespace Clusters {
namespace Aggregate {

class AggregateClusterTest : public testing::Test {
public:
  AggregateClusterTest() : stats_(Upstream::ClusterInfoImpl::generateStats(stats_store_)) {}

  Upstream::HostVector setupHostSet(int healthy_hosts, int degraded_hosts, int unhealthy_hosts) {
    Upstream::HostVector hosts;
    for (int i = 0; i < healthy_hosts; ++i) {
      hosts.emplace_back(Upstream::makeTestHost(info_, "tcp://127.0.0.1:80"));
    }

    for (int i = 0; i < degraded_hosts; ++i) {
      Upstream::HostSharedPtr host = Upstream::makeTestHost(info_, "tcp://127.0.0.2:80");
      host->healthFlagSet(Upstream::HostImpl::HealthFlag::DEGRADED_ACTIVE_HC);
      hosts.emplace_back(host);
    }

    for (int i = 0; i < unhealthy_hosts; ++i) {
      Upstream::HostSharedPtr host = Upstream::makeTestHost(info_, "tcp://127.0.0.3:80");
      host->healthFlagSet(Upstream::HostImpl::HealthFlag::FAILED_ACTIVE_HC);
      hosts.emplace_back(host);
    }

    return hosts;
  }

  void setupPrimary(int priority, int healthy_hosts, int degraded_hosts, int unhealthy_hosts) {
    auto hosts = setupHostSet(healthy_hosts, degraded_hosts, unhealthy_hosts);
    primary_ps_.updateHosts(
        priority,
        Upstream::HostSetImpl::partitionHosts(std::make_shared<Upstream::HostVector>(hosts),
                                              Upstream::HostsPerLocalityImpl::empty()),
        nullptr, hosts, {}, 100);
    cluster_->refresh();
  }

  void setupSecondary(int priority, int healthy_hosts, int degraded_hosts, int unhealthy_hosts) {
    auto hosts = setupHostSet(healthy_hosts, degraded_hosts, unhealthy_hosts);
    secondary_ps_.updateHosts(
        priority,
        Upstream::HostSetImpl::partitionHosts(std::make_shared<Upstream::HostVector>(hosts),
                                              Upstream::HostsPerLocalityImpl::empty()),
        nullptr, hosts, {}, 100);
    cluster_->refresh();
  }

  void setupPrioritySet() {
    setupPrimary(0, 1, 1, 1);
    setupPrimary(1, 2, 2, 2);
    setupSecondary(0, 2, 2, 2);
    setupSecondary(1, 1, 1, 1);
  }

  void initialize(const std::string& yaml_config) {
    envoy::config::cluster::v3::Cluster cluster_config =
        Upstream::parseClusterFromV2Yaml(yaml_config);
    envoy::extensions::clusters::aggregate::v3::ClusterConfig config;
    Config::Utility::translateOpaqueConfig(cluster_config.cluster_type().typed_config(),
                                           ProtobufWkt::Struct::default_instance(),
                                           ProtobufMessage::getStrictValidationVisitor(), config);
    Stats::ScopePtr scope = stats_store_.createScope("cluster.name.");
    Server::Configuration::TransportSocketFactoryContextImpl factory_context(
        admin_, ssl_context_manager_, *scope, cm_, local_info_, dispatcher_, random_, stats_store_,
        singleton_manager_, tls_, validation_visitor_, *api_);

    cluster_ = std::make_shared<Cluster>(cluster_config, config, cm_, runtime_, random_,
                                         factory_context, std::move(scope), tls_, false);

    thread_aware_lb_ = std::make_unique<AggregateThreadAwareLoadBalancer>(*cluster_);
    lb_factory_ = thread_aware_lb_->factory();
    lb_ = lb_factory_->create();

    EXPECT_CALL(cm_, get(Eq("aggregate_cluster"))).WillRepeatedly(Return(&aggregate_cluster_));
    EXPECT_CALL(cm_, get(Eq("primary"))).WillRepeatedly(Return(&primary_));
    EXPECT_CALL(cm_, get(Eq("secondary"))).WillRepeatedly(Return(&secondary_));
    EXPECT_CALL(cm_, get(Eq("tertiary"))).WillRepeatedly(Return(nullptr));
    ON_CALL(primary_, prioritySet()).WillByDefault(ReturnRef(primary_ps_));
    ON_CALL(secondary_, prioritySet()).WillByDefault(ReturnRef(secondary_ps_));
    ON_CALL(aggregate_cluster_, loadBalancer()).WillByDefault(ReturnRef(*lb_));

    setupPrioritySet();

    ON_CALL(primary_, loadBalancer()).WillByDefault(ReturnRef(primary_load_balancer_));
    ON_CALL(secondary_, loadBalancer()).WillByDefault(ReturnRef(secondary_load_balancer_));
  }

  Stats::IsolatedStoreImpl stats_store_;
  Ssl::MockContextManager ssl_context_manager_;
  NiceMock<Upstream::MockClusterManager> cm_;
  NiceMock<Runtime::MockRandomGenerator> random_;
  NiceMock<ThreadLocal::MockInstance> tls_;
  NiceMock<Runtime::MockLoader> runtime_;
  NiceMock<Event::MockDispatcher> dispatcher_;
  NiceMock<LocalInfo::MockLocalInfo> local_info_;
  NiceMock<Server::MockAdmin> admin_;
  Singleton::ManagerImpl singleton_manager_{Thread::threadFactoryForTest()};
  NiceMock<ProtobufMessage::MockValidationVisitor> validation_visitor_;
  Api::ApiPtr api_{Api::createApiForTest(stats_store_)};
  std::shared_ptr<Cluster> cluster_;
  Upstream::ThreadAwareLoadBalancerPtr thread_aware_lb_;
  Upstream::LoadBalancerFactorySharedPtr lb_factory_;
  Upstream::LoadBalancerPtr lb_;
  Upstream::ClusterStats stats_;
  std::shared_ptr<Upstream::MockClusterInfo> info_{new NiceMock<Upstream::MockClusterInfo>()};
  NiceMock<Upstream::MockThreadLocalCluster> aggregate_cluster_, primary_, secondary_;
  Upstream::PrioritySetImpl primary_ps_, secondary_ps_;
  NiceMock<Upstream::MockLoadBalancer> primary_load_balancer_, secondary_load_balancer_;

  const std::string default_yaml_config_ = R"EOF(
    name: aggregate_cluster
    connect_timeout: 0.25s
    lb_policy: CLUSTER_PROVIDED
    cluster_type:
      name: envoy.clusters.aggregate
      typed_config:
        "@type": type.googleapis.com/envoy.config.cluster.aggregate.v2alpha.ClusterConfig
        clusters:
        - primary
        - secondary
)EOF";
}; // namespace Aggregate

TEST_F(AggregateClusterTest, LoadBalancerTest) {
  initialize(default_yaml_config_);
  // Health value:
  // Cluster 1:
  //     Priority 0: 33.3%
  //     Priority 1: 33.3%
  // Cluster 2:
  //     Priority 0: 33.3%
  //     Priority 1: 33.3%
  Upstream::HostSharedPtr host = Upstream::makeTestHost(info_, "tcp://127.0.0.1:80");
  EXPECT_CALL(primary_load_balancer_, chooseHost(_)).WillRepeatedly(Return(host));
  EXPECT_CALL(secondary_load_balancer_, chooseHost(_)).WillRepeatedly(Return(nullptr));

  for (int i = 0; i <= 65; ++i) {
    EXPECT_CALL(random_, random()).WillOnce(Return(i));
    Upstream::HostConstSharedPtr target = lb_->chooseHost(nullptr);
    EXPECT_EQ(host.get(), target.get());
  }

  EXPECT_CALL(primary_load_balancer_, chooseHost(_)).WillRepeatedly(Return(nullptr));
  EXPECT_CALL(secondary_load_balancer_, chooseHost(_)).WillRepeatedly(Return(host));
  for (int i = 66; i < 100; ++i) {
    EXPECT_CALL(random_, random()).WillOnce(Return(i));
    Upstream::HostConstSharedPtr target = lb_->chooseHost(nullptr);
    EXPECT_EQ(host.get(), target.get());
  }

  // Set up the HostSet with 1 healthy, 1 degraded and 2 unhealthy.
  setupPrimary(0, 1, 1, 2);

  // Health value:
  // Cluster 1:
  //     Priority 0: 25%
  //     Priority 1: 33.3%
  // Cluster 2:
  //     Priority 0: 33.3%
  //     Priority 1: 33.3%
  EXPECT_CALL(primary_load_balancer_, chooseHost(_)).WillRepeatedly(Return(host));
  EXPECT_CALL(secondary_load_balancer_, chooseHost(_)).WillRepeatedly(Return(nullptr));

  for (int i = 0; i <= 57; ++i) {
    EXPECT_CALL(random_, random()).WillOnce(Return(i));
    Upstream::HostConstSharedPtr target = lb_->chooseHost(nullptr);
    EXPECT_EQ(host.get(), target.get());
  }

  EXPECT_CALL(primary_load_balancer_, chooseHost(_)).WillRepeatedly(Return(nullptr));
  EXPECT_CALL(secondary_load_balancer_, chooseHost(_)).WillRepeatedly(Return(host));
  for (int i = 58; i < 100; ++i) {
    EXPECT_CALL(random_, random()).WillOnce(Return(i));
    Upstream::HostConstSharedPtr target = lb_->chooseHost(nullptr);
    EXPECT_EQ(host.get(), target.get());
  }
}

TEST_F(AggregateClusterTest, AllHostAreUnhealthyTest) {
  initialize(default_yaml_config_);
  Upstream::HostSharedPtr host = Upstream::makeTestHost(info_, "tcp://127.0.0.1:80");
  // Set up the HostSet with 0 healthy, 0 degraded and 2 unhealthy.
  setupPrimary(0, 0, 0, 2);
  setupPrimary(1, 0, 0, 2);

  // Set up the HostSet with 0 healthy, 0 degraded and 2 unhealthy.
  setupSecondary(0, 0, 0, 2);
  setupSecondary(1, 0, 0, 2);
  // Health value:
  // Cluster 1:
  //     Priority 0: 0%
  //     Priority 1: 0%
  // Cluster 2:
  //     Priority 0: 0%
  //     Priority 1: 0%
  EXPECT_CALL(primary_load_balancer_, chooseHost(_)).WillRepeatedly(Return(host));
  EXPECT_CALL(secondary_load_balancer_, chooseHost(_)).WillRepeatedly(Return(nullptr));

  // Choose the first cluster as the second one is unavailable.
  for (int i = 0; i < 50; ++i) {
    EXPECT_CALL(random_, random()).WillOnce(Return(i));
    Upstream::HostConstSharedPtr target = lb_->chooseHost(nullptr);
    EXPECT_EQ(host.get(), target.get());
  }

  EXPECT_CALL(primary_load_balancer_, chooseHost(_)).WillRepeatedly(Return(nullptr));
  EXPECT_CALL(secondary_load_balancer_, chooseHost(_)).WillRepeatedly(Return(host));

  // Choose the second cluster as the first one is unavailable.
  for (int i = 50; i < 100; ++i) {
    EXPECT_CALL(random_, random()).WillOnce(Return(i));
    Upstream::HostConstSharedPtr target = lb_->chooseHost(nullptr);
    EXPECT_EQ(host.get(), target.get());
  }
}

TEST_F(AggregateClusterTest, ClusterInPanicTest) {
  initialize(default_yaml_config_);
  Upstream::HostSharedPtr host = Upstream::makeTestHost(info_, "tcp://127.0.0.1:80");
  setupPrimary(0, 1, 0, 4);
  setupPrimary(1, 1, 0, 4);
  setupSecondary(0, 1, 0, 4);
  setupSecondary(1, 1, 0, 4);
  // Health value:
  // Cluster 1:
  //     Priority 0: 20%
  //     Priority 1: 20%
  // Cluster 2:
  //     Priority 0: 20%
  //     Priority 1: 20%
  // All priorities are in panic mode. Traffic will be distributed evenly among four priorities.
  EXPECT_CALL(primary_load_balancer_, chooseHost(_)).WillRepeatedly(Return(host));
  EXPECT_CALL(secondary_load_balancer_, chooseHost(_)).WillRepeatedly(Return(nullptr));

  for (int i = 0; i < 50; ++i) {
    EXPECT_CALL(random_, random()).WillOnce(Return(i));
    Upstream::HostConstSharedPtr target = lb_->chooseHost(nullptr);
    EXPECT_EQ(host.get(), target.get());
  }

  EXPECT_CALL(primary_load_balancer_, chooseHost(_)).WillRepeatedly(Return(nullptr));
  EXPECT_CALL(secondary_load_balancer_, chooseHost(_)).WillRepeatedly(Return(host));

  for (int i = 50; i < 100; ++i) {
    EXPECT_CALL(random_, random()).WillOnce(Return(i));
    Upstream::HostConstSharedPtr target = lb_->chooseHost(nullptr);
    EXPECT_EQ(host.get(), target.get());
  }

  setupPrimary(0, 1, 0, 9);
  setupPrimary(1, 1, 0, 9);
  setupSecondary(0, 1, 0, 9);
  setupSecondary(1, 1, 0, 1);
  // Health value:
  // Cluster 1:
  //     Priority 0: 10%
  //     Priority 1: 10%
  // Cluster 2:
  //     Priority 0: 10%
  //     Priority 0: 50%
  EXPECT_CALL(primary_load_balancer_, chooseHost(_)).WillRepeatedly(Return(host));
  EXPECT_CALL(secondary_load_balancer_, chooseHost(_)).WillRepeatedly(Return(nullptr));

  for (int i = 0; i <= 25; ++i) {
    EXPECT_CALL(random_, random()).WillOnce(Return(i));
    Upstream::HostConstSharedPtr target = lb_->chooseHost(nullptr);
    EXPECT_EQ(host.get(), target.get());
  }

  EXPECT_CALL(primary_load_balancer_, chooseHost(_)).WillRepeatedly(Return(nullptr));
  EXPECT_CALL(secondary_load_balancer_, chooseHost(_)).WillRepeatedly(Return(host));

  for (int i = 26; i < 100; ++i) {
    EXPECT_CALL(random_, random()).WillOnce(Return(i));
    Upstream::HostConstSharedPtr target = lb_->chooseHost(nullptr);
    EXPECT_EQ(host.get(), target.get());
  }
}

TEST_F(AggregateClusterTest, LBContextTest) {
  AggregateLoadBalancerContext context(nullptr,
                                       Upstream::LoadBalancerBase::HostAvailability::Healthy, 0);

  EXPECT_EQ(context.computeHashKey().has_value(), false);
  EXPECT_EQ(context.downstreamConnection(), nullptr);
  EXPECT_EQ(context.metadataMatchCriteria(), nullptr);
  EXPECT_EQ(context.downstreamHeaders(), nullptr);
  EXPECT_EQ(context.upstreamSocketOptions(), nullptr);
  EXPECT_EQ(context.upstreamTransportSocketOptions(), nullptr);
}

} // namespace Aggregate
} // namespace Clusters
} // namespace Extensions
} // namespace Envoy
