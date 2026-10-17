#include "dynamic-common.hpp"
#include <algorithm>
#include <set>
#include <string_view>

int main(int argc,char** argv) try {
  using namespace dynamic204;
  Options options(argc,argv);
  const auto mode=options.get("--mode"),session=options.get("--session"),originPath=options.get("--origin");
  const bool participationStudy = options.number("--participation-study", 0) == 1;
  if (participationStudy && mode != "SOLICIT")
    throw std::invalid_argument("participation study requires SOLICIT");
  if(mode!="SOLICIT" && mode!="ADS100" && mode!="ADS1000" && mode!="ADS_EVENT"
      && mode!="ADS_COARSE")
    throw std::invalid_argument("mode");
  const ndn::Name identity("/example/hello/user"),controller("/example/hello/controller"),service("/HELLO");
  const ndn::Name group(options.get("--group"));
  const std::vector<ndn::Name> providers{ndn::Name("/example/hello/provider/A"),ndn::Name("/example/hello/provider/B")};
  const auto windowMs=options.number("--window-ms",60000);
  const auto periodMs=options.number("--period-ms",4700);
  if(periodMs!=100 && periodMs!=1000 && periodMs!=4700)
    throw std::invalid_argument("unsupported period");
  if(windowMs<1000 || windowMs>60000 || session.empty()) throw std::invalid_argument("window/session");
  std::ifstream planFile(options.get("--task-plan"));
  if (!planFile) throw std::invalid_argument("missing frozen task plan");
  std::string planText(131073, '\0');
  planFile.read(planText.data(), planText.size());
  planText.resize(static_cast<size_t>(planFile.gcount()));
  if (planText.size() > 131072) throw std::invalid_argument("task plan too large");
  const auto taskPlan = decodeTaskPlan(Json::parse(planText), windowMs, periodMs);
  Records records(options.get("--records"));
  // Record only authenticated public binding metadata, never token key material.
  const auto responseBinding = [](const nsf::ResponseMessage& response) {
    Json binding = Json::object();
    for (const auto* field : {"targeted.providerBootEpoch",
                              "targeted.registrationGeneration", "targeted.expiresAtMs"}) {
      const auto found = response.getTokens().find(field);
      binding[field] = found == response.getTokens().end() ? Json(nullptr) : Json(found->second);
    }
    return binding;
  };
  ndn::Face face;ndn::KeyChain keys;ndn::Scheduler scheduler(face.getIoContext());
  const auto cert=certificate(keys,identity);keys.setDefaultIdentity(keys.getPib().getIdentity(identity));
  nsf::CertificatePublisher publisher(face,keys,identity);
  std::map<ndn::Name,ndn::security::Certificate> certs;
  for(const auto& name:providers)certs.emplace(name,certificate(keys,name));
  std::map<ndn::Name,Snapshot> cache;
  std::map<ndn::Name,Snapshot> statusCache;
  uint64_t invalid=0,sequence=0,terminals=0,t0=0,t1=0;
  bool started=false,draining=false,warmReady=false;
  auto accept=[&](const ndn::Data& data,const ndn::Name& producer,const char* source) {
    try {
      const bool coarseAd=mode=="ADS_COARSE" && std::string_view(source)=="SVS-advertisement";
      auto& destination=mode=="ADS_COARSE" && std::string_view(source)=="fresh-status"
        ? statusCache : cache;
      const auto s=verifySnapshot(data,certs.at(producer),session,nowNs(),
                                  coarseAd ? COARSE_AD_TTL_NS : TTL_NS);
      const auto old=destination.find(producer);
      if(old!=destination.end() && s.sequence<=old->second.sequence) {
        records.write({{"event","advertisement_rejected"},{"reason","old_or_duplicate_sequence"},
                       {"provider",producer.toUri()},{"mono_ns",nowNs()}});return;
      }
      destination.insert_or_assign(producer,s);
      records.write({{"event","snapshot_accepted"},{"source",source},{"snapshot",snapshotJson(s)},
                     {"mono_ns",nowNs()}});
    }catch(const std::exception& e) {
      // Malformed/stale input is rejected, never used for selection.
      records.write({{"event","advertisement_rejected"},{"reason",e.what()},
                     {"provider",producer.toUri()},{"mono_ns",nowNs()}});
    }
  };
  std::unique_ptr<AdvertisementBus> discovery;
  if(mode!="SOLICIT") discovery=std::make_unique<AdvertisementBus>(group,identity,session,face,keys,cert,
    [&](const ndn::svs::SVSPubSub::SubscriptionData& publication) {
      // Copy off the library callback; cache ownership remains on the Face IO.
      if(publication.data.empty() || publication.data.size()>8192)return;
      ndn::Buffer owned(publication.data.begin(),publication.data.end());
      boost::asio::post(face.getIoContext(),[&,owned=std::move(owned)] {
        try {
          const auto& bytes=owned;
          if(bytes.empty() || bytes.size()>8192)throw std::runtime_error("advertisement wire length");
          ndn::Block block(ndn::span<const uint8_t>{bytes.data(),bytes.size()});
          if(block.size()!=bytes.size())throw std::runtime_error("trailing inner wire bytes");
          ndn::Data data(block);
          ndn::Name producer;
          for(const auto& p:providers)if(statusPrefix(p).isPrefixOf(data.getName()))producer=p;
          if(producer.empty())throw std::runtime_error("unexpected advertisement producer");
          accept(data,producer,"SVS-advertisement");
        }catch(const std::exception& e) {
          records.write({{"event","advertisement_rejected"},{"reason",e.what()},{"mono_ns",nowNs()}});
        }
      });
    });
  struct Refresh {
    int pending = 2;
    bool resumed = false;
    ndn::scheduler::ScopedEventId timer;
    std::vector<ndn::ScopedPendingInterestHandle> interests;
  };
  struct Task {
    uint64_t sequence,begin,deadline;std::string command;int attempts=0;
    TaskDescription description{1, false, false};
    std::set<ndn::Name> unansweredTargets;
    int successAttempt=0;std::string successProvider,successRequestId;uint64_t successResponseNs=0;
    bool terminal=false;ndn::scheduler::ScopedEventId timer;
    std::shared_ptr<Refresh> refreshOwner;
  };
  struct Attempt {
    std::shared_ptr<Task> task;int index;uint64_t begin,deadline;
    bool settled=false;ndn::Name target;ndn::scheduler::EventId timer;
    std::mutex selectionMutex;ndn::Name selected;bool ackDecisionComplete=false;
    ndn::Name requestId;
  };
  std::function<void()> next;
  std::function<void(std::shared_ptr<Task>)> submit,refresh;
  std::function<void(std::shared_ptr<Task>,std::string)> finish;
  std::function<void(std::shared_ptr<Attempt>,const nsf::ResponseMessage*)> settle;
  std::shared_ptr<Task> activeTask;
  size_t arrivalIndex = 0;
  ndn::scheduler::ScopedEventId arrivalTimer;
  ndn::scheduler::ScopedEventId drainTimer;
  // Native workers join before all callback-owned experiment state is released.
  {
    nsf::ServiceUser user(face,group,cert,certificate(keys,controller),options.get("--trust"));
    user.init();user.setUseTokens(true);user.fetchPermissionsFromController(controller);
    finish=[&](const std::shared_ptr<Task>& task,std::string outcome) {
      if(task->terminal)return;
      const auto now=nowNs();
      if(outcome=="success" && now>=task->deadline)outcome="timeout";
      task->terminal=true;task->timer.cancel();++terminals;
      task->refreshOwner.reset();
      if (activeTask == task) activeTask.reset();
      records.write({{"event","terminal"},{"sequence",task->sequence},{"command",task->command},
                     {"mode",mode},{"begin_ns",task->begin},{"deadline_ns",task->deadline},
                     {"terminal_ns",now},{"terminal",outcome},{"attempts",task->attempts},
                     {"success_attempt",task->successAttempt},{"success_provider",task->successProvider},
                     {"success_request_id",task->successRequestId},{"success_response_ns",task->successResponseNs}});
    };
    refresh=[&](const std::shared_ptr<Task>& task) {
      if(task->terminal)return;
      const auto now=nowNs();if(now>=task->deadline){finish(task,"timeout");return;}
      auto round=std::make_shared<Refresh>();
      task->refreshOwner = round;
      const std::weak_ptr<Task> weakTask = task;
      const std::weak_ptr<Refresh> weakRound = round;
      auto resume=[&,weakTask,weakRound] {
        const auto task = weakTask.lock();
        const auto round = weakRound.lock();
        if (!task || !round || task->terminal || task->refreshOwner != round) return;
        if(round->resumed)return;round->resumed=true;round->timer.cancel();
        round->interests.clear();
        task->refreshOwner.reset();
        if(!task->terminal)submit(task);
      };
      const auto budget=std::min<uint64_t>(120,(task->deadline-now)/1000000);
      if (!budget) {
        // Less than one millisecond remains; the owned deadline timer closes the task.
        task->refreshOwner.reset();
        return;
      }
      round->timer=scheduler.schedule(ndn::time::milliseconds(budget),resume);
      for(const auto& producer:providers) {
        ndn::Interest interest(ndn::Name(statusPrefix(producer)).append(session)
          .appendNumber(task->sequence).appendNumber(task->attempts).appendVersion(now));
        interest.setMustBeFresh(true);interest.setInterestLifetime(ndn::time::milliseconds(budget));
        records.write({{"event","refresh_interest"},{"provider",producer.toUri()},
                       {"sequence",task->sequence},{"mono_ns",nowNs()}});
        auto complete = [weakRound,resume] {
          if (const auto round = weakRound.lock()) {
            if (!round->resumed && --round->pending == 0) resume();
          }
        };
        round->interests.emplace_back(face.expressInterest(interest,
          [&,producer,weakTask,weakRound,complete](const auto&,const ndn::Data& data) {
            const auto task = weakTask.lock();
            const auto round = weakRound.lock();
            if (!task || !round || task->terminal || task->refreshOwner != round) return;
            accept(data,producer,"fresh-status");complete();
          },[complete](const auto&,const auto&) {complete();},
          [complete](const auto&) {complete();}));
      }
    };
    settle=[&](const std::shared_ptr<Attempt>& attempt,const nsf::ResponseMessage* response) {
      const auto task=attempt->task;
      bool success=false;
      bool explicitNonExecution=false;
      if(response) {
        const auto name=ndn::Name(response->getProviderIdentity());
        ndn::Name expected=attempt->target;
        if(mode=="SOLICIT") {
          std::lock_guard<std::mutex> lock(attempt->selectionMutex);expected=attempt->selected;
        }
        const bool identityOk=certs.count(name) && !response->getDataName().empty()
          && name.isPrefixOf(ndn::Name(response->getDataName())) && !response->getWireDigest().empty()
          && !expected.empty() && expected==name;
        const bool echo=stringPayload(response->getPayload())==task->command;
        success=response->getStatus() && identityOk && echo;
        explicitNonExecution=!response->getStatus() && identityOk && echo
          && response->getErrorInfo()=="task ineligible at admission";
        const bool contradicts=(!response->getProviderIdentity().empty()
          && (!certs.count(name) || expected.empty() || expected!=name))
          || (!response->getDataName().empty()
              && (name.empty() || !name.isPrefixOf(ndn::Name(response->getDataName()))));
        if(contradicts || (response->getStatus() && !success))++invalid;
        const auto observed=nowNs();
        records.write({{"event","attempt_response"},{"sequence",task->sequence},{"attempt",attempt->index},
          {"provider",response->getProviderIdentity()},{"status",response->getStatus()},
          {"request_id",attempt->requestId.toUri()},
          {"identity_verified",identityOk},{"echo_verified",echo},
          {"late",attempt->settled || task->terminal ||
            observed>=(mode=="SOLICIT" ? task->deadline : attempt->deadline)},
          {"error",response->getErrorInfo()},{"mono_ns",observed},
          {"response_binding",responseBinding(*response)}});
      }
      if(attempt->settled)return;
      bool executionPossible=false;
      if(mode=="SOLICIT") {
        std::lock_guard<std::mutex> lock(attempt->selectionMutex);
        executionPossible=!attempt->ackDecisionComplete || !attempt->selected.empty();
      }
      const auto now=nowNs();
      if(mode=="SOLICIT" && !response && executionPossible && now<task->deadline
         && !task->terminal) {
        // A local timeout cannot prove that a selected Provider did not execute.
        attempt->timer.cancel();
        attempt->timer=scheduler.schedule(
          ndn::time::nanoseconds(task->deadline-now),[&,attempt]{settle(attempt,nullptr);});
        records.write({{"event","uncertain_execution_wait"},{"sequence",task->sequence},
          {"attempt",attempt->index},{"request_id",attempt->requestId.toUri()},
          {"deadline_ns",task->deadline},{"mono_ns",now}});
        return;
      }
      if(mode=="ADS_COARSE" && !response && !attempt->target.empty())
        task->unansweredTargets.insert(attempt->target);
      attempt->settled=true;attempt->timer.cancel();
      if(task->terminal)return;
      if(success && now<(mode=="SOLICIT" ? task->deadline : attempt->deadline)) {
        task->successAttempt=attempt->index;task->successProvider=response->getProviderIdentity();
        task->successRequestId=attempt->requestId.toUri();task->successResponseNs=now;
        finish(task,"success");return;
      }
      if(mode=="SOLICIT" && executionPossible && !explicitNonExecution) {
        finish(task,"uncertain_execution");return;
      }
      if(now>=task->deadline){finish(task,"timeout");return;}
      if(task->attempts>=3){finish(task,"attempts_exhausted");return;}
      if(mode=="SOLICIT")submit(task);else refresh(task);
    };
    submit=[&](const std::shared_ptr<Task>& task) {
      if(task->terminal)return;
      const auto now=nowNs();
      if(now>=task->deadline){finish(task,"timeout");return;}
      if(task->attempts>=3){finish(task,"attempts_exhausted");return;}
      auto attempt=std::make_shared<Attempt>();attempt->task=task;attempt->index=task->attempts+1;
      attempt->begin=now;attempt->deadline=std::min<uint64_t>(task->deadline,now+600000000ULL);
      std::map<ndn::Name,Snapshot> visible;
      if(mode!="SOLICIT") {
        int bestScore = 0;
        int untriedScore = 0;
        ndn::Name untried;
        visible=cache;
        if(mode=="ADS_COARSE") {
          for(const auto& entry:statusCache) {
            if(entry.second.expires>now) visible.insert_or_assign(entry.first,entry.second);
          }
        }
        for(const auto& entry:visible) {
          const auto& s=entry.second;if(!s.available || s.expires<=now)continue;
          const int score = taskScore(task->description, s.available,
                                      s.computeUnits, s.memoryUnits);
          if (score > bestScore) {
            bestScore = score;
            attempt->target = entry.first;
          }
          if (score > untriedScore && !task->unansweredTargets.count(entry.first)) {
            untriedScore = score;
            untried = entry.first;
          }
        }
        if (!untried.empty()) attempt->target = untried;
        if(attempt->target.empty()) {
          records.write({{"event","no_cached_candidate"},{"sequence",task->sequence},{"attempt",attempt->index},{"mono_ns",now}});
          refresh(task);return;
        }
      }
      const int budget=std::max<int>(1,(attempt->deadline-now)/1000000);
      ++task->attempts;
      attempt->timer=scheduler.schedule(ndn::time::milliseconds(budget),[&,attempt]{settle(attempt,nullptr);});
      nsf::RequestMessage request;
      ndn::Buffer payload(reinterpret_cast<const uint8_t*>(task->command.data()),task->command.size());
      request.setPayload(payload,payload.size());
      const auto descriptor = taskJson(task->description).dump();
      request.setDiscoveryPayload(ndn::Buffer(
        reinterpret_cast<const uint8_t*>(descriptor.data()), descriptor.size()));
      auto timeout=[&,attempt](const ndn::Name&) {boost::asio::post(face.getIoContext(),[&,attempt]{settle(attempt,nullptr);});};
      auto response=[&,attempt](const nsf::ResponseMessage& response) {
        boost::asio::post(face.getIoContext(),[&,attempt,response]{settle(attempt,&response);});
      };
      try {
        ndn::Name id;
        if(mode=="SOLICIT") id=user.RequestService(providers,service,request,std::min(250,budget),
          nsf::ServiceUser::AckCandidatesHandler([&,attempt](const std::vector<nsf::AckSelectionCandidate>& candidates) {
            std::vector<nsf::AckSelectionCandidate> selected;
            Json observed=Json::array();
            for(const auto& c:candidates) {
              observed.push_back({{"provider",c.providerName.toUri()},{"authenticated",c.authenticationEvidence.trustSchemaValidated},
                {"signer",c.authenticationEvidence.signerIdentity},{"status",c.ack.getStatus()},{"message",c.ack.getMessage()}});
              try {
                const auto j=Json::parse(c.ack.getMessage());
                const auto observedNs = unsignedField(j,"observed_ns");
                const auto selectionNow = nowNs();
                if(c.authenticationEvidence.trustSchemaValidated
                   && c.authenticationEvidence.signerIdentity==c.providerName.toUri() && certs.count(c.providerName)
                   && c.ack.getStatus() && j.at("task") == taskJson(attempt->task->description)
                   && observedNs <= selectionNow && selectionNow - observedNs < TTL_NS
                   && (participationStudy ||
                       taskScore(attempt->task->description, j.at("available").get<bool>(),
                                 resourceUnits(j,"compute_units"),
                                 resourceUnits(j,"memory_units")) > 0)) selected.push_back(c);
              }catch(...){}
            }
            std::sort(selected.begin(),selected.end(),[&](const auto& a,const auto& b) {
              if (participationStudy) return a.providerName < b.providerName;
              const auto left = Json::parse(a.ack.getMessage());
              const auto right = Json::parse(b.ack.getMessage());
              const auto leftScore = taskScore(attempt->task->description, true,
                resourceUnits(left,"compute_units"), resourceUnits(left,"memory_units"));
              const auto rightScore = taskScore(attempt->task->description, true,
                resourceUnits(right,"compute_units"), resourceUnits(right,"memory_units"));
              return leftScore == rightScore ? a.providerName < b.providerName : leftScore > rightScore;
            });
            if(selected.size()>1)selected.resize(1);
            {std::lock_guard<std::mutex> lock(attempt->selectionMutex);
             attempt->ackDecisionComplete=true;
             if(!selected.empty())attempt->selected=selected.front().providerName;}
            records.write({{"event","ack_selection"},{"sequence",attempt->task->sequence},{"attempt",attempt->index},
              {"candidates",observed},{"selected",selected.empty()?"":selected.front().providerName.toUri()},{"mono_ns",nowNs()}});
            return selected;
          }),std::max<int>(1,(task->deadline-now)/1000000),timeout,response,
          nsf::tlv::FirstResponding);
        else id=user.RequestServiceTargeted(attempt->target,service,request,budget,timeout,response);
        attempt->requestId=id;
        records.write({{"event","attempt_submitted"},{"sequence",task->sequence},{"attempt",attempt->index},
          {"request_id",id.toUri()},{"target",attempt->target.toUri()},{"begin_ns",now},
          {"deadline_ns",attempt->deadline}});
        if(!attempt->target.empty()) {
          Json observations = Json::array();
          for (const auto& entry : visible) observations.push_back(snapshotJson(entry.second));
          records.write({{"event","advertisement_selection"},
            {"sequence",task->sequence},{"attempt",attempt->index},
            {"snapshot",snapshotJson(visible.at(attempt->target))},{"candidates",observations},
            {"mono_ns",nowNs()}});
        }
        if(id.empty())settle(attempt,nullptr);
      }catch(const std::exception& e) {
        records.write({{"event","submit_exception"},{"error",e.what()},{"mono_ns",nowNs()}});settle(attempt,nullptr);
      }
    };
    next=[&] {
      while (arrivalIndex < taskPlan.size()
             && t0 + taskPlan[arrivalIndex].arrivalMs * 1000000 <= nowNs()) {
        const auto& planned = taskPlan[arrivalIndex++];
        auto task = std::make_shared<Task>();
        task->sequence = ++sequence;
        task->description = planned.description;
        task->begin = t0 + planned.arrivalMs * 1000000;
        task->deadline = task->begin + 2000000000ULL;
        task->command = taskInput(task->description);
        const auto dispatchNow = nowNs();
        records.write({{"event","arrival"},{"sequence",task->sequence},
          {"task",taskJson(task->description)},{"planned_ns",task->begin},{"dispatch_ns",dispatchNow}});
        if (activeTask && !activeTask->terminal && dispatchNow >= activeTask->deadline) {
          const auto expired = activeTask;
          finish(expired,"timeout");
        }
        if (dispatchNow >= task->deadline) {
          finish(task,"timeout");
        }
        else if (activeTask && !activeTask->terminal) {
          finish(task,"rejected");
        }
        else {
          activeTask = task;
          const auto waitMs = (task->deadline - dispatchNow + 999999) / 1000000;
          const std::weak_ptr<Task> weakTask = task;
          task->timer = scheduler.schedule(ndn::time::milliseconds(waitMs),[&,weakTask] {
            if (const auto task = weakTask.lock()) finish(task,"timeout");
          });
          submit(task);
        }
      }
      if(arrivalIndex == taskPlan.size() && nowNs()>=t1) {
        if(!draining) {
          draining=true;records.write({{"event","cohort_closed"},{"t0_ns",t0},{"t1_ns",t1},
            {"tasks",sequence},{"terminals",terminals},{"invalid",invalid},{"mono_ns",nowNs()}});
          const auto drainEnd = t1 + 3000000000ULL;
          const auto drainNow = nowNs();
          const auto drainMs = drainEnd > drainNow ? (drainEnd - drainNow + 999999) / 1000000 : 0;
          drainTimer = scheduler.schedule(ndn::time::milliseconds(drainMs),[&]{shutdownFace(face);});
        }return;
      }
      const auto due = arrivalIndex < taskPlan.size()
        ? t0 + taskPlan[arrivalIndex].arrivalMs * 1000000 : t1;
      const auto afterDispatch = nowNs();
      const auto waitMs = due > afterDispatch ? (due - afterDispatch + 999999) / 1000000 : 0;
      arrivalTimer = scheduler.schedule(ndn::time::milliseconds(waitMs),[&]{next();});
    };
    const auto readyStart=Clock::now();int warmed=0;bool warming=false;
    std::function<void()> check;
    check=[&] {
      if(!warmReady && Clock::now()-readyStart>std::chrono::seconds(45)) {
        records.write({{"event","warmup_readiness_timeout"}});shutdownFace(face);return;
      }
      if(!user.isRequestBootstrapReady(service,providers)) {
        if(Clock::now()-readyStart>std::chrono::seconds(45)) {shutdownFace(face);return;}
      }else if(warmed<2 && !warming) {
        warming=true;nsf::RequestMessage request;
        const TaskDescription warmTask{static_cast<uint64_t>(warmed + 1), false, true};
        const auto warmInput = taskInput(warmTask);
        ndn::Buffer payload(reinterpret_cast<const uint8_t*>(warmInput.data()), warmInput.size());
        request.setPayload(payload,payload.size());
        const auto descriptor = taskJson(warmTask).dump();
        request.setDiscoveryPayload(ndn::Buffer(
          reinterpret_cast<const uint8_t*>(descriptor.data()), descriptor.size()));
        const auto target=providers.at(warmed);
        const auto warmId=user.RequestServiceTargeted(target,service,request,3000,
          [&](const ndn::Name&){boost::asio::post(face.getIoContext(),[&]{++invalid;shutdownFace(face);});},
          [&,target,warmInput](const nsf::ResponseMessage& response) {
            boost::asio::post(face.getIoContext(),[&,target,response,warmInput] {
              if(!response.getStatus() || response.getProviderIdentity()!=target.toUri()
                 || stringPayload(response.getPayload())!=warmInput){++invalid;shutdownFace(face);return;}
              records.write({{"event", "warmup_response"}, {"provider", target.toUri()},
                             {"mono_ns", nowNs()}, {"response_binding", responseBinding(response)}});
              ++warmed;warming=false;
            });
          });
        if(warmId.empty()) {
          records.write({{"event","warmup_submit_rejected"},{"provider",target.toUri()}});
          ++invalid;shutdownFace(face);return;
        }
      }else if(warmed==2) {
        if(!warmReady){warmReady=true;std::cout<<"SPEC204_USER_WARM_READY"<<std::endl;}
        const auto origin=readOrigin(originPath);
        if(origin && nowNs()>=origin) {
          started=true;t0=origin;t1=t0+uint64_t(windowMs)*1000000;
          records.write({{"event","window"},{"t0_ns",t0},{"t1_ns",t1},{"mode",mode},{"period_ms",periodMs}});next();return;
        }
      }
      scheduler.schedule(ndn::time::milliseconds(20),check);
    };
    scheduler.schedule(ndn::time::milliseconds(20),check);
    boost::asio::signal_set signals(face.getIoContext(),SIGINT,SIGTERM);
    signals.async_wait([&](const auto&,int){shutdownFace(face);});face.processEvents();
    arrivalTimer.cancel();
    drainTimer.cancel();
    if (activeTask) {
      activeTask->timer.cancel();
      activeTask->refreshOwner.reset();
      activeTask->terminal = true;
    }
  }
  discovery.reset();records.write({{"event","shutdown"},{"started",started},{"tasks",sequence},
    {"planned_tasks",taskPlan.size()},{"terminals",terminals},{"invalid",invalid}});
  return started && draining && sequence==taskPlan.size() && sequence==terminals && !invalid?0:2;
}catch(const std::exception& e){std::cerr<<"SPEC204_USER_ERROR "<<e.what()<<std::endl;return 2;}
