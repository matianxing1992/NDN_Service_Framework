#include "dynamic-common.hpp"
#include <atomic>
#include <thread>
#include <tuple>

int main(int argc,char** argv) try {
  using namespace dynamic204;
  Options options(argc,argv);
  const ndn::Name identity(options.get("--identity")), controller(options.get("--controller","/example/hello/controller"));
  const ndn::Name service("/HELLO"),group(options.get("--group"));
  const auto session=options.get("--session"),mode=options.get("--mode"),originPath=options.get("--origin");
  const auto recoveryCondition = options.get("--recovery-condition", "none");
  const int providerIndex = options.number("--index", 0);
  const bool taskBusy = options.number("--task-busy", 0) == 1;
  const bool ackAll = options.number("--ack-all", 0) == 1;
  if (ackAll && (mode != "SOLICIT" || !taskBusy))
    throw std::invalid_argument("ACK-all requires SOLICIT task-busy study");
  if (recoveryCondition != "none" && recoveryCondition != "prepared"
      && recoveryCondition != "reregister")
    throw std::invalid_argument("recovery condition");
  if(identity.empty() || group.empty() || session.empty() || session.find_first_not_of("0123456789")!=std::string::npos)
    throw std::invalid_argument("identity/group/decimal session");
  if(mode!="SOLICIT" && mode!="ADS100" && mode!="ADS1000" && mode!="ADS_EVENT"
      && mode!="ADS_COARSE")
    throw std::invalid_argument("mode");
  if (taskBusy && ((mode != "SOLICIT" && mode != "ADS_COARSE")
                   || providerIndex < 0 || providerIndex > 1))
    throw std::invalid_argument("task-busy mode/index");
  std::vector<AvailabilityChange> changes;
  const auto tracePath = options.get("--availability-trace");
  if (!tracePath.empty()) {
    std::ifstream input(tracePath, std::ios::binary);
    if (!input) throw std::invalid_argument("availability trace missing");
    std::string bytes(65537, '\0');
    input.read(bytes.data(), bytes.size());
    const auto count = input.gcount();
    if (count > 65536 || (!input.eof() && input.fail()))
      throw std::invalid_argument("availability trace size/read");
    bytes.resize(static_cast<size_t>(count));
    changes = decodeAvailabilityTrace(parseAvailabilityTrace(bytes), identity,
                                       options.number("--window-ms", 60000));
    const ndn::Name expected(providerIndex == 0 ? "/example/hello/provider/A"
                                      : "/example/hello/provider/B");
    if (identity != expected) throw std::invalid_argument("trace Provider/index mismatch");
  }
  if (taskBusy && changes.empty()) throw std::invalid_argument("task-busy trace required");
  LocalState state(providerIndex, options.number("--dwell-ms", 0),
                   options.number("--force-unavailable", 0) == 1, std::move(changes));
  auto taskCapacity = [&](uint64_t now) {
    int compute = state.computeUnits(), memory = state.memoryUnits();
    const bool full = state.available(now);
    if (taskBusy && !full) {
      if (providerIndex == 0) compute = 3;
      else memory = 3;
    }
    return std::tuple<bool, int, int>{taskBusy || full, compute, memory};
  };
  const auto handlerMs=options.number("--handler-ms",5);
  if(handlerMs!=5 && handlerMs!=80 && handlerMs!=800)
    throw std::invalid_argument("handler duration profile");
  Records records(options.get("--records"));
  ndn::Face face; ndn::KeyChain keys; ndn::Scheduler scheduler(face.getIoContext());
  const auto cert=certificate(keys,identity); keys.setDefaultIdentity(keys.getPib().getIdentity(identity));
  nsf::CertificatePublisher certPublisher(face,keys,identity);
  uint64_t seq=0;
  auto sample=[&] {
    const auto now=nowNs();
    const auto [available, compute, memory] = taskCapacity(now);
    return Snapshot{identity,session,++seq,now,now+TTL_NS,available,compute,memory};
  };
  auto coarseSample=[&] {
    const auto now=nowNs();
    return Snapshot{identity,session,++seq,now,now+COARSE_AD_TTL_NS,true,
                    state.computeUnits(),state.memoryUnits()};
  };
  auto signedSnapshot=[&](const ndn::Name& name,const Snapshot& s) {
    ndn::Data data(name); data.setFreshnessPeriod(ndn::time::milliseconds(0));
    data.setContent(std::string_view(snapshotJson(s).dump()));
    keys.sign(data,ndn::security::signingByCertificate(cert)); return data;
  };
  const auto refreshRegistration=face.setInterestFilter(statusPrefix(identity),
    [&](const ndn::InterestFilter&,const ndn::Interest& interest) {
      const auto s=sample(); auto data=signedSnapshot(interest.getName(),s); face.put(data);
      records.write({{"event","fresh_status"},{"provider",identity.toUri()},
                     {"snapshot",snapshotJson(s)},{"mono_ns",nowNs()}});
    },[](const ndn::Name&,const std::string& error){throw std::runtime_error("status registration: "+error);});
  std::unique_ptr<AdvertisementBus> discovery;
  if(mode!="SOLICIT") discovery=std::make_unique<AdvertisementBus>(group,identity,session,face,keys,cert,
    [](const ndn::svs::SVSPubSub::SubscriptionData&){});
  bool ready=false,advertised=false,lastAvailable=true;
  bool prepared = false, preparationFailed = false;
  uint64_t lastPublished=0;
  std::function<void()> tick;
  tick=[&] {
    state.setOrigin(readOrigin(originPath));
    const auto now=nowNs(); const bool available=state.available(now);
    const uint64_t cadence=(mode=="ADS100"?100:1000)*1000000ULL;
    if(ready && discovery && (!advertised || (mode!="ADS_COARSE"
                              && (now-lastPublished>=cadence
                                  || (mode=="ADS_EVENT" && available!=lastAvailable))))) {
      const auto s=mode=="ADS_COARSE" ? coarseSample() : sample();
      auto data=signedSnapshot(ndn::Name(statusPrefix(identity)).append(session)
                               .appendVersion(s.sequence),s);
      discovery->publish(data,identity,session,s.sequence);
      advertised=true;lastPublished=now;lastAvailable=available;
      records.write({{"event","advertisement"},{"snapshot",snapshotJson(s)},{"mono_ns",nowNs()}});
    }
    scheduler.schedule(ndn::time::milliseconds(5),tick);
  };
  // Provider is destroyed before callback state and discovery dependencies.
  {
    nsf::ServiceProvider provider(face,group,cert,certificate(keys,controller),options.get("--trust"));
    provider.setUseTokens(true);
    auto registerService = [&] {
      // Each handler owns its registration identity, including during retirement.
      auto generation = std::make_shared<std::atomic<uint64_t>>(0);
      auto handle=provider.addScopedService(service,
      nsf::ServiceProvider::AckStrategyHandler([&](const nsf::RequestMessage& request) {
        // ACK workers only observe immutable trace parameters and atomic epoch.
        const auto now=nowNs();
        const auto [available, compute, memory] = taskCapacity(now);
        Json j{{"available",available},{"compute_units",compute},
               {"memory_units",memory},{"observed_ns",now}};
        nsf::ServiceProvider::AckDecision decision;
        decision.status = false;
        try {
          const auto task = decodeTask(stringPayload(request.getPayload()));
          j["task"] = taskJson(task);
          decision.status = ackAll || taskScore(task, available, compute, memory) > 0;
        }
        catch (const std::exception& error) {
          j["error"] = error.what();
        }
        decision.message=j.dump();
        records.write({{"event","ack_handler"},{"provider",identity.toUri()},{"state",j},{"mono_ns",now}});
        return decision;
      }),nsf::ServiceProvider::RequestHandler([&, generation](const ndn::Name&,const ndn::Name&,const ndn::Name&,
          const ndn::Name& requestId,const nsf::RequestMessage& request) {
        const auto begin=nowNs();auto payload=request.getPayload();
        bool eligible = false;
        bool valid = false;
        Json taskRecord = nullptr;
        std::string inputError;
        const auto [available, compute, memory] = taskCapacity(begin);
        try {
          const auto task = decodeTaskInput(stringPayload(payload));
          taskRecord = taskJson(task);
          valid = true;
          eligible = taskScore(task, available, compute, memory) > 0;
        }
        catch (const std::exception& error) {
          inputError = error.what();
        }
        if(eligible && valid) std::this_thread::sleep_for(std::chrono::milliseconds(handlerMs));
        records.write({{"event","execution"},{"provider",identity.toUri()},{"request_id",requestId.toUri()},
                       {"registration_generation", generation->load()},
                       {"request_mode", request.getRequestMode()},
                       {"command",stringPayload(payload)},{"task",taskRecord},
                       {"eligible",eligible},{"payload_valid",valid},
                       {"available",available},
                       {"compute_units",compute},{"memory_units",memory},
                       {"begin_ns",begin},{"end_ns",nowNs()}});
        nsf::ResponseMessage response;response.setStatus(eligible && valid);
        response.setErrorInfo(!valid ? inputError : eligible ? "" : "task ineligible at admission");
        response.setPayload(payload,payload.size());return response;
      }),nsf::ServiceProvider::ServiceInvocationMode::NormalAndTargeted);
      const auto value = handle.generation();
      if (value == 0) throw std::runtime_error("zero registration generation");
      generation->store(value);
      return handle;
    };
    auto registration = registerService();
    provider.init();provider.fetchPermissionsFromController(controller);
    const auto start=Clock::now();std::function<void()> check;
    check=[&] {
      if(provider.hasProviderPermissionForService(service) && provider.isRequestDecryptionReady()) {
        ready=true;records.write({{"event","ready"},{"provider",identity.toUri()},{"mono_ns",nowNs()}});
        std::cout<<"SPEC204_PROVIDER_READY "<<identity.toUri()<<std::endl;
      } else if(Clock::now()-start>std::chrono::seconds(45)) shutdownFace(face);
      else scheduler.schedule(ndn::time::milliseconds(100),check);
    };
    scheduler.schedule(ndn::time::milliseconds(100),check);scheduler.schedule(ndn::time::milliseconds(5),tick);
    boost::asio::signal_set signals(face.getIoContext(),SIGINT,SIGTERM,SIGUSR1);
    std::function<void()> waitSignal;
    waitSignal = [&] {
      signals.async_wait([&](const auto& error, int signal) {
        if (error) {
          if (error != boost::asio::error::operation_aborted) {
            preparationFailed = true;
            records.write({{"event", "signal_failed"}, {"error", error.message()}});
            shutdownFace(face);
          }
          return;
        }
        if (signal != SIGUSR1) {
          shutdownFace(face);
          return;
        }
        try {
          if (!ready || prepared || recoveryCondition == "none" || readOrigin(originPath) != 0)
            throw std::runtime_error("invalid recovery preparation boundary");
          const auto before = registration.generation();
          if (recoveryCondition == "reregister") {
            registration.close();
            registration = registerService();
          }
          const auto after = registration.generation();
          if (before == 0 || (recoveryCondition == "prepared" ? after != before : after <= before))
            throw std::runtime_error("invalid recovery registration generation");
          records.write({{"event", "recovery_prepared"}, {"provider", identity.toUri()},
                         {"condition", recoveryCondition}, {"before_generation", before},
                         {"after_generation", after}, {"mono_ns", nowNs()}});
          prepared = true;
          std::cout << "SPEC205_RECOVERY_PREPARED " << identity.toUri() << std::endl;
          waitSignal();
        }
        catch (const std::exception& failure) {
          preparationFailed = true;
          records.write({{"event", "recovery_preparation_failed"}, {"error", failure.what()}});
          shutdownFace(face);
        }
      });
    };
    waitSignal();
    face.processEvents();
  }
  const bool completed = ready && !preparationFailed
    && (recoveryCondition == "none" || prepared);
  discovery.reset();records.write({{"event","shutdown"},{"ready",ready},
                                  {"preparation_complete", completed}});
  return completed ? 0 : 2;
}catch(const std::exception& e){std::cerr<<"SPEC204_PROVIDER_ERROR "<<e.what()<<std::endl;return 2;}
