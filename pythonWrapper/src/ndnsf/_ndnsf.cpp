#include "ndn-service-framework/CertificatePublisher.hpp"
#include "ndn-service-framework/ServiceProvider.hpp"
#include "ndn-service-framework/ServiceController.hpp"
#include "ndn-service-framework/ServiceUser.hpp"

#include <ndn-cxx/face.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/key-params.hpp>
#include <ndn-cxx/security/validator-config.hpp>

#include <pybind11/functional.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace py = pybind11;
namespace nsf = ndn_service_framework;

namespace {

std::mutex g_keyChainMutex;

using PyFunctionPtr = std::shared_ptr<py::function>;

PyFunctionPtr
keepPyFunction(py::function fn)
{
  return PyFunctionPtr(new py::function(std::move(fn)), [](py::function* value) {
    py::gil_scoped_acquire gil;
    delete value;
  });
}

void
processFaceEvents(ndn::Face& face, ndn::time::milliseconds timeout)
{
  // ndn-cxx stops the io_context when processEvents(timeout) returns by
  // timeout. Python roles pump the Face repeatedly, so restart before each
  // bounded pump to keep later Interests/Data moving.
  face.getIoContext().restart();
  face.processEvents(timeout);
}

ndn::security::Certificate
getOrCreateIdentity(ndn::KeyChain& keyChain, const ndn::Name& identity)
{
  std::lock_guard<std::mutex> lock(g_keyChainMutex);
  try {
    return keyChain.getPib()
      .getIdentity(identity)
      .getDefaultKey()
      .getDefaultCertificate();
  }
  catch (const std::exception&) {
    return keyChain.createIdentity(identity, ndn::RsaKeyParams(2048))
      .getDefaultKey()
      .getDefaultCertificate();
  }
}

ndn::Buffer
toBuffer(const py::bytes& value)
{
  const std::string bytes = value;
  return ndn::Buffer(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
}

py::bytes
toPyBytes(const ndn::Buffer& value)
{
  return py::bytes(reinterpret_cast<const char*>(value.data()), value.size());
}

std::shared_ptr<const nsf::AckSelectionPolicy>
selectionPolicyByName(const std::string& strategy)
{
  if (strategy == "all-selected" || strategy == "all-responders") {
    return nsf::strategy::AllSelected;
  }
  if (strategy == "random-selection" || strategy == "load-balancing") {
    return nsf::strategy::RandomSelection;
  }
  return nsf::strategy::FirstResponding;
}

struct PyServiceResponse
{
  bool status = false;
  py::bytes payload;
  std::string error;
};

struct PyAckDecision
{
  bool status = true;
  py::bytes payload;
  std::string message = "ok";
  bool suppress = false;
};

class NativeServiceProvider
{
public:
  NativeServiceProvider(const std::string& providerId,
                        const std::string& group,
                        const std::string& controller,
                        const std::string& providerPrefix,
                        const std::string& trustSchema,
                        size_t handlerThreads,
                        size_t ackThreads,
                        bool serveCertificates)
    : m_group(group)
    , m_controller(controller)
    , m_providerPrefix(providerPrefix)
    , m_providerIdentity(providerId.empty() ? m_providerPrefix : ndn::Name(m_providerPrefix).append(providerId))
    , m_trustSchema(trustSchema)
    , m_handlerThreads(handlerThreads)
    , m_ackThreads(ackThreads)
    , m_serveCertificates(serveCertificates)
  {
    m_providerCert = getOrCreateIdentity(m_keyChain, m_providerIdentity);
    m_controllerCert = getOrCreateIdentity(m_keyChain, m_controller);
    {
      std::lock_guard<std::mutex> lock(g_keyChainMutex);
      m_keyChain.setDefaultIdentity(m_keyChain.getPib().getIdentity(m_providerIdentity));
    }
    if (m_serveCertificates) {
      m_certPublisher = std::make_unique<nsf::CertificatePublisher>(
        m_face, m_keyChain, m_providerCert.getName());
    }
    m_provider = std::make_unique<nsf::ServiceProvider>(
      m_face, m_group, m_providerCert, m_controllerCert, m_trustSchema);
    m_provider->setPerformanceMode(true);
    m_provider->setUseTokens(true);
    m_provider->setHandlerThreads(m_handlerThreads);
    m_provider->setAckThreads(m_ackThreads);
  }

  ~NativeServiceProvider()
  {
    stop();
  }

  void
  addService(const std::string& serviceName,
             py::function requestHandler,
             std::optional<py::function> ackHandler)
  {
    if (!m_provider) {
      throw std::runtime_error("provider is not initialized");
    }
    m_handlers.emplace(serviceName, requestHandler);
    if (ackHandler) {
      m_ackHandlers.emplace(serviceName, *ackHandler);
    }

    m_provider->addService(
      ndn::Name(serviceName),
      nsf::ServiceProvider::AckStrategyHandler(
        [this, serviceName](const nsf::RequestMessage& request) {
          nsf::ServiceProvider::AckDecision decision;
          auto it = m_ackHandlers.find(serviceName);
          if (it == m_ackHandlers.end()) {
            decision.status = true;
            decision.message = "python-provider-ready";
            return decision;
          }
          py::gil_scoped_acquire gil;
          try {
            py::object result = it->second(toPyBytes(request.getPayload()));
            if (py::isinstance<PyAckDecision>(result)) {
              auto pyDecision = result.cast<PyAckDecision>();
              decision.status = pyDecision.status;
              decision.suppressAck = pyDecision.suppress;
              decision.message = pyDecision.message;
              decision.payload = toBuffer(pyDecision.payload);
            }
            else {
              decision.status = result.cast<bool>();
              decision.message = decision.status ? "python-provider-ready" : "python-provider-rejected";
            }
          }
          catch (const py::error_already_set& e) {
            decision.status = false;
            decision.suppressAck = true;
            decision.message = e.what();
          }
          return decision;
        }),
      nsf::ServiceProvider::RequestHandler(
        [this, serviceName](const ndn::Name&,
                            const ndn::Name&,
                            const ndn::Name&,
                            const ndn::Name&,
                            const nsf::RequestMessage& request) {
          nsf::ResponseMessage response;
          py::gil_scoped_acquire gil;
          try {
            py::object result = m_handlers.at(serviceName)(toPyBytes(request.getPayload()));
            if (py::isinstance<PyServiceResponse>(result)) {
              auto pyResponse = result.cast<PyServiceResponse>();
              response.setStatus(pyResponse.status);
              response.setErrorInfo(pyResponse.error.empty() ? "No error" : pyResponse.error);
              auto payload = toBuffer(pyResponse.payload);
              response.setPayload(payload, payload.size());
            }
            else {
              auto payload = toBuffer(result.cast<py::bytes>());
              response.setStatus(true);
              response.setErrorInfo("No error");
              response.setPayload(payload, payload.size());
            }
          }
          catch (const py::error_already_set& e) {
            response.setStatus(false);
            response.setErrorInfo(e.what());
          }
          return response;
        }));
  }

  void
  start()
  {
    if (m_running.exchange(true)) {
      return;
    }
    m_provider->init();
    m_provider->fetchPermissionsFromController(m_controller);
    m_thread = std::thread([this] {
      while (m_running.load()) {
        try {
          processFaceEvents(m_face, ndn::time::milliseconds(10));
        }
        catch (const std::exception& e) {
          std::lock_guard<std::mutex> lock(m_errorMutex);
          m_error = e.what();
          m_running = false;
        }
      }
    });
  }

  void
  run()
  {
    start();
    while (m_running.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      throwIfError();
    }
  }

  void
  stop()
  {
    m_running = false;
    if (m_thread.joinable()) {
      m_thread.join();
    }
  }

  void
  throwIfError()
  {
    std::lock_guard<std::mutex> lock(m_errorMutex);
    if (!m_error.empty()) {
      throw std::runtime_error(m_error);
    }
  }

private:
  ndn::Face m_face;
  ndn::KeyChain m_keyChain;
  ndn::Name m_group;
  ndn::Name m_controller;
  ndn::Name m_providerPrefix;
  ndn::Name m_providerIdentity;
  std::string m_trustSchema;
  size_t m_handlerThreads = 4;
  size_t m_ackThreads = 2;
  bool m_serveCertificates = true;
  ndn::security::Certificate m_providerCert;
  ndn::security::Certificate m_controllerCert;
  std::unique_ptr<nsf::CertificatePublisher> m_certPublisher;
  std::unique_ptr<nsf::ServiceProvider> m_provider;
  std::map<std::string, py::function> m_handlers;
  std::map<std::string, py::function> m_ackHandlers;
  std::atomic<bool> m_running{false};
  std::thread m_thread;
  std::mutex m_errorMutex;
  std::string m_error;
};

class NativeServiceController
{
public:
  NativeServiceController(const std::string& controllerPrefix,
                          const std::string& policyFile,
                          const std::string& trustSchema,
                          const std::vector<std::string>& bootstrapIdentities,
                          bool serveCertificates)
    : m_controllerPrefix(controllerPrefix)
    , m_policyFile(policyFile)
    , m_trustSchema(trustSchema)
    , m_validator(m_face)
    , m_serveCertificates(serveCertificates)
  {
    m_controllerCert = getOrCreateIdentity(m_keyChain, m_controllerPrefix);
    {
      std::lock_guard<std::mutex> lock(g_keyChainMutex);
      m_keyChain.setDefaultIdentity(m_keyChain.getPib().getIdentity(m_controllerPrefix));
    }
    for (const auto& identity : bootstrapIdentities) {
      if (!identity.empty()) {
        getOrCreateIdentity(m_keyChain, ndn::Name(identity));
      }
    }
    if (!m_trustSchema.empty()) {
      m_validator.load(m_trustSchema);
    }
    if (m_serveCertificates) {
      m_certPublisher = std::make_unique<nsf::CertificatePublisher>(
        m_face, m_keyChain, m_controllerCert.getName());
    }
    m_controller = std::make_unique<nsf::ServiceController>(
      m_face, m_controllerCert, m_validator, m_policyFile);
    m_controller->setControllerPrefix(m_controllerPrefix);
  }

  ~NativeServiceController()
  {
    stop();
  }

  void
  start()
  {
    if (m_running.exchange(true)) {
      return;
    }
    m_thread = std::thread([this] {
      try {
        m_controller->run();
      }
      catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(m_errorMutex);
        m_error = e.what();
      }
      m_running = false;
    });
  }

  void
  run()
  {
    if (m_running.exchange(true)) {
      return;
    }
    try {
      m_controller->run();
    }
    catch (const std::exception& e) {
      {
        std::lock_guard<std::mutex> lock(m_errorMutex);
        m_error = e.what();
      }
      m_running = false;
      throw;
    }
    m_running = false;
  }

  void
  stop()
  {
    m_running = false;
    m_face.shutdown();
    m_face.getIoContext().stop();
    if (m_thread.joinable()) {
      m_thread.join();
    }
  }

  void
  throwIfError()
  {
    std::lock_guard<std::mutex> lock(m_errorMutex);
    if (!m_error.empty()) {
      throw std::runtime_error(m_error);
    }
  }

private:
  ndn::Face m_face;
  ndn::KeyChain m_keyChain;
  ndn::Name m_controllerPrefix;
  std::string m_policyFile;
  std::string m_trustSchema;
  ndn::ValidatorConfig m_validator;
  bool m_serveCertificates = true;
  ndn::security::Certificate m_controllerCert;
  std::unique_ptr<nsf::CertificatePublisher> m_certPublisher;
  std::unique_ptr<nsf::ServiceController> m_controller;
  std::atomic<bool> m_running{false};
  std::thread m_thread;
  std::mutex m_errorMutex;
  std::string m_error;
};

class NativeServiceUser
{
public:
  NativeServiceUser(const std::string& group,
                    const std::string& controller,
                    const std::string& userIdentity,
                    const std::string& trustSchema,
                    int permissionWaitMs,
                    size_t handlerThreads,
                    size_t ackThreads,
                    bool adaptiveAdmission,
                    bool serveCertificates)
    : m_group(group)
    , m_controller(controller)
    , m_userIdentity(userIdentity)
    , m_trustSchema(trustSchema)
    , m_permissionWaitMs(permissionWaitMs)
  {
    m_userCert = getOrCreateIdentity(m_keyChain, m_userIdentity);
    m_controllerCert = getOrCreateIdentity(m_keyChain, m_controller);
    {
      std::lock_guard<std::mutex> lock(g_keyChainMutex);
      m_keyChain.setDefaultIdentity(m_keyChain.getPib().getIdentity(m_userIdentity));
    }
    if (serveCertificates) {
      m_certPublisher = std::make_unique<nsf::CertificatePublisher>(
        m_face, m_keyChain, m_userCert.getName());
    }
    m_user = std::make_unique<nsf::ServiceUser>(
      m_face, m_group, m_userCert, m_controllerCert, m_trustSchema);
    m_user->setPerformanceMode(true);
    m_user->setUseTokens(true);
    m_user->setHandlerThreads(handlerThreads);
    m_user->setAckProcessingThreads(ackThreads);
    nsf::ServiceUser::AdaptiveAdmissionOptions admission;
    admission.enabled = adaptiveAdmission;
    m_user->setAdaptiveAdmissionControl(admission);
    m_user->fetchPermissionsFromController(m_controller);
    m_user->init();
    pump(m_permissionWaitMs);
  }

  ~NativeServiceUser()
  {
    stop();
  }

  void
  start()
  {
    if (m_running.exchange(true)) {
      return;
    }
    m_thread = std::thread([this] {
      while (m_running.load()) {
        try {
          processFaceEvents(m_face, ndn::time::milliseconds(10));
        }
        catch (const std::exception& e) {
          std::lock_guard<std::mutex> lock(m_errorMutex);
          m_error = e.what();
          m_running = false;
        }
      }
    });
  }

  void
  stop()
  {
    m_running = false;
    if (m_thread.joinable()) {
      m_thread.join();
    }
  }

  void
  throwIfError()
  {
    std::lock_guard<std::mutex> lock(m_errorMutex);
    if (!m_error.empty()) {
      throw std::runtime_error(m_error);
    }
  }

  PyServiceResponse
  requestService(const std::string& serviceName,
                 const py::bytes& requestPayload,
                 int ackTimeoutMs,
                 int timeoutMs,
                 const std::string& strategy)
  {
    PyServiceResponse output;
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;

    auto payload = toBuffer(requestPayload);
    auto selection = selectionPolicyByName(strategy);

    auto submit = [&, payload, selection] {
      m_user->RequestService(
        ndn::Name(serviceName),
        payload,
        ackTimeoutMs,
        selection,
        timeoutMs,
        [&](const nsf::ResponseMessage& response) {
          py::gil_scoped_acquire gil;
          std::lock_guard<std::mutex> lock(mutex);
          output.status = response.getStatus();
          output.payload = toPyBytes(response.getPayload());
          output.error = response.getErrorInfo();
          done = true;
          cv.notify_one();
        },
        [&](const ndn::Name& requestId) {
          std::lock_guard<std::mutex> lock(mutex);
          output.status = false;
          output.error = "timeout: " + requestId.toUri();
          done = true;
          cv.notify_one();
        });
    };

    if (m_running.load()) {
      m_face.getIoContext().post(submit);
      const auto deadline = std::chrono::steady_clock::now() +
                            std::chrono::milliseconds(timeoutMs + 3000);
      py::gil_scoped_release release;
      std::unique_lock<std::mutex> lock(mutex);
      cv.wait_until(lock, deadline, [&done] { return done; });
      if (done) {
        return output;
      }
      output.status = false;
      output.error = "local deadline";
      return output;
    }

    submit();

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs + 3000);
    while (std::chrono::steady_clock::now() < deadline) {
      {
        std::lock_guard<std::mutex> lock(mutex);
        if (done) {
          return output;
        }
      }
      py::gil_scoped_release release;
      processFaceEvents(m_face, ndn::time::milliseconds(10));
    }
    output.status = false;
    output.error = "local deadline";
    return output;
  }

  void
  requestServiceAsync(const std::string& serviceName,
                      const py::bytes& requestPayload,
                      py::function onResponse,
                      py::function onTimeout,
                      int ackTimeoutMs,
                      int timeoutMs,
                      const std::string& strategy)
  {
    start();
    auto payload = toBuffer(requestPayload);
    auto selection = selectionPolicyByName(strategy);
    auto responseCallback = keepPyFunction(std::move(onResponse));
    auto timeoutCallback = keepPyFunction(std::move(onTimeout));
    m_face.getIoContext().post(
      [this, serviceName, payload, selection, ackTimeoutMs, timeoutMs,
       responseCallback = std::move(responseCallback),
       timeoutCallback = std::move(timeoutCallback)] {
        m_user->RequestService(
          ndn::Name(serviceName),
          payload,
          ackTimeoutMs,
          selection,
          timeoutMs,
          [responseCallback](const nsf::ResponseMessage& response) mutable {
            py::gil_scoped_acquire gil;
            PyServiceResponse output;
            output.status = response.getStatus();
            output.payload = toPyBytes(response.getPayload());
            output.error = response.getErrorInfo();
            try {
              (*responseCallback)(output);
            }
            catch (const py::error_already_set& e) {
              PyErr_WriteUnraisable(e.value().ptr());
            }
          },
          [timeoutCallback](const ndn::Name& requestId) mutable {
            py::gil_scoped_acquire gil;
            try {
              (*timeoutCallback)(requestId.toUri());
            }
            catch (const py::error_already_set& e) {
              PyErr_WriteUnraisable(e.value().ptr());
            }
          });
      });
  }

  void
  pump(int milliseconds)
  {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(milliseconds);
    while (std::chrono::steady_clock::now() < deadline) {
      processFaceEvents(m_face, ndn::time::milliseconds(10));
    }
  }

  std::vector<std::tuple<std::string, std::string, std::string>>
  getAllowedServices() const
  {
    return m_user->getAllowedServices();
  }

private:
  ndn::Face m_face;
  ndn::KeyChain m_keyChain;
  ndn::Name m_group;
  ndn::Name m_controller;
  ndn::Name m_userIdentity;
  std::string m_trustSchema;
  int m_permissionWaitMs = 1500;
  ndn::security::Certificate m_userCert;
  ndn::security::Certificate m_controllerCert;
  std::unique_ptr<nsf::CertificatePublisher> m_certPublisher;
  std::unique_ptr<nsf::ServiceUser> m_user;
  std::atomic<bool> m_running{false};
  std::thread m_thread;
  std::mutex m_errorMutex;
  std::string m_error;
};

} // namespace

PYBIND11_MODULE(_ndnsf, m)
{
  py::class_<PyServiceResponse>(m, "ServiceResponse")
    .def(py::init<>())
    .def_readwrite("status", &PyServiceResponse::status)
    .def_readwrite("payload", &PyServiceResponse::payload)
    .def_readwrite("error", &PyServiceResponse::error);

  py::class_<PyAckDecision>(m, "AckDecision")
    .def(py::init<>())
    .def_readwrite("status", &PyAckDecision::status)
    .def_readwrite("payload", &PyAckDecision::payload)
    .def_readwrite("message", &PyAckDecision::message)
    .def_readwrite("suppress", &PyAckDecision::suppress);

  py::class_<NativeServiceController>(m, "NativeServiceController")
    .def(py::init<const std::string&,
                  const std::string&,
                  const std::string&,
                  const std::vector<std::string>&,
                  bool>(),
         py::arg("controller_prefix") = "/example/hello/controller",
         py::arg("policy_file") = "examples/hello.policies",
         py::arg("trust_schema") = "examples/trust-schema.conf",
         py::arg("bootstrap_identities") = std::vector<std::string>{},
         py::arg("serve_certificates") = true)
    .def("start", &NativeServiceController::start)
    .def("run", &NativeServiceController::run, py::call_guard<py::gil_scoped_release>())
    .def("stop", &NativeServiceController::stop);

  py::class_<NativeServiceProvider>(m, "NativeServiceProvider")
    .def(py::init<const std::string&,
                  const std::string&,
                  const std::string&,
                  const std::string&,
                  const std::string&,
                  size_t,
                  size_t,
                  bool>(),
         py::arg("provider_id") = "",
         py::arg("group") = "/example/hello/group",
         py::arg("controller") = "/example/hello/controller",
         py::arg("provider_prefix") = "/example/hello/provider",
         py::arg("trust_schema") = "examples/trust-schema.conf",
         py::arg("handler_threads") = 4,
         py::arg("ack_threads") = 2,
         py::arg("serve_certificates") = true)
    .def("add_service", &NativeServiceProvider::addService,
         py::arg("service"),
         py::arg("request_handler"),
         py::arg("ack_handler") = std::optional<py::function>())
    .def("start", &NativeServiceProvider::start)
    .def("run", &NativeServiceProvider::run, py::call_guard<py::gil_scoped_release>())
    .def("stop", &NativeServiceProvider::stop);

  py::class_<NativeServiceUser>(m, "NativeServiceUser")
    .def(py::init<const std::string&,
                  const std::string&,
                  const std::string&,
                  const std::string&,
                  int,
                  size_t,
                  size_t,
                  bool,
                  bool>(),
         py::arg("group") = "/example/hello/group",
         py::arg("controller") = "/example/hello/controller",
         py::arg("user") = "/example/hello/user",
         py::arg("trust_schema") = "examples/trust-schema.conf",
         py::arg("permission_wait_ms") = 1500,
         py::arg("handler_threads") = 2,
         py::arg("ack_threads") = 2,
         py::arg("adaptive_admission") = false,
         py::arg("serve_certificates") = true)
    .def("request_service", &NativeServiceUser::requestService,
         py::arg("service"),
         py::arg("payload"),
         py::arg("ack_timeout_ms") = 300,
         py::arg("timeout_ms") = 5000,
         py::arg("strategy") = "first-responding")
    .def("request_service_async", &NativeServiceUser::requestServiceAsync,
         py::arg("service"),
         py::arg("payload"),
         py::arg("on_response"),
         py::arg("on_timeout"),
         py::arg("ack_timeout_ms") = 300,
         py::arg("timeout_ms") = 5000,
         py::arg("strategy") = "first-responding")
    .def("start", &NativeServiceUser::start)
    .def("stop", &NativeServiceUser::stop)
    .def("get_allowed_services", &NativeServiceUser::getAllowedServices)
    .def("pump", &NativeServiceUser::pump);
}
