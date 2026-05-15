````md
# AGENTS.md

# ndn_service_framework - Codex Instructions

## Task Scope

This task is limited to the CodeGenerator part of the project.

Only modify files under:

```text
CodeGenerator/
````

Especially focus on:

```text
CodeGenerator/Generated/User.hpp
CodeGenerator/Generated/Provider.hpp
```

```
Try to make the standalone CodeGenerator/Generated/User.hpp and CodeGenerator/Generated/Provider.hpp capable of handling all functionality by themselves, without relying on additional Python-generated code anymore.
```

Do not modify unrelated framework source files unless absolutely necessary.

---

## Project Context

This project is an NDN-based C++ service framework.

The old design generates service-specific C++ classes such as:

```text
ServiceProvider_Drone.hpp
ServiceUser_Drone.hpp
```

These classes inherit from classes inside `NDN_SERVICE_FRAMEWORK`.

The new goal is to improve the generated generic classes:

```text
CodeGenerator/Generated/User.hpp
CodeGenerator/Generated/Provider.hpp
```

so that they do not need to inherit from any `NDN_SERVICE_FRAMEWORK` base classes, but can still provide the same functionality as the old generated service-specific classes.

---

## Main Goal

Modify the generated `User.hpp` and `Provider.hpp` design so that:

1. `CodeGenerator/Generated/User.hpp` works like the old generated `ServiceUser_Drone.hpp`.
2. `CodeGenerator/Generated/Provider.hpp` works like the old generated `ServiceProvider_Drone.hpp`.
3. They must not inherit from old framework base classes.
4. They should be self-contained C++ classes.
5. Existing service invocation behavior should remain compatible.
6. Existing NDN naming behavior should remain compatible.
7. Existing request/response serialization should remain compatible.
8. Existing security, certificate, and NAC-ABE related logic should not be removed or broken.

---

## Important Existing Files

Study these files before making changes:

```text
CodeGenerator/Generated/User.hpp
CodeGenerator/Generated/Provider.hpp
```

Also compare against old generated classes such as:

```text
ServiceProvider_Drone.hpp
ServiceUser_Drone.hpp
```

or any similarly generated service-specific provider/user classes.

The goal is to move the useful logic from those old service-specific classes into the new generic generated classes.

---

## Generated Code Rule

Files under:

```text
CodeGenerator/Generated/
```

are generated files.

Prefer modifying the generator or templates instead of manually editing generated output.

If manual edits are made only for quick prototyping, also update the generator/templates so the same result can be reproduced by running:

```bash
cd CodeGenerator
sudo python NDNSFCodeGenerator.py
```

---

## Code Generator Structure

The code generator directory contains:

```text
CodeGenerator/
  Generated/
  Template/
  app.yml
  NDNSFCodeGenerator.py
```

`app.yml` defines:

* services
* RPC methods
* request message types
* response message types

`NDNSFCodeGenerator.py` reads `app.yml` and templates from `Template/`, then outputs generated C++ files into `Generated/`.

---

## Required Behavior for Provider.hpp

`CodeGenerator/Generated/Provider.hpp` should provide the functionality previously provided by old service provider classes such as `ServiceProvider_Drone.hpp`.

It should support:

* provider-side NDN service initialization
* service registration
* request receiving
* request name parsing
* request payload decoding
* dispatching to the correct service method handler
* handler assignment by user code
* response creation
* response serialization
* response publishing/sending
* ACK behavior if the old provider class supported it
* compatibility with existing examples where possible

It should allow application code to define handlers in a style similar to:

```cpp
provider.SomeService.SomeMethod_Handler =
    [&](const ndn::Name& requesterIdentity,
        const RequestType& request,
        ResponseType& response)
{
    // service logic
};
```

or an equivalent clean dynamic registration style.

Do not require inheritance from old `NDN_SERVICE_FRAMEWORK` provider base classes.

---

## Required Behavior for User.hpp

`CodeGenerator/Generated/User.hpp` should provide the functionality previously provided by old service user classes such as `ServiceUser_Drone.hpp`.

It should support:

* user-side NDN service initialization
* request construction
* request serialization
* service invocation
* sending Interests or service requests
* receiving responses
* response decoding
* callback-based or synchronous behavior if the old class supported it
* compatibility with existing examples where possible

It should allow application code to invoke services in a style equivalent to the old generated user classes.

Do not require inheritance from old `NDN_SERVICE_FRAMEWORK` user base classes.

---

## Compatibility Requirements

Preserve compatibility with existing behavior unless explicitly impossible.

Do not change without explanation:

* NDN Interest name format
* Data name format
* service name format
* method name format
* protobuf message types
* request/response serialization format
* certificate logic
* NAC-ABE encryption/decryption logic
* ACK message behavior
* provider selection behavior

If any change is required, explain why and keep the change as small as possible.

---

## Migration Style

Do not rewrite the whole project.

Use an incremental migration:

1. Analyze old generated provider/user classes.
2. Identify what logic is inherited from `NDN_SERVICE_FRAMEWORK`.
3. Copy or reimplement only the needed behavior inside generated `Provider.hpp` and `User.hpp`.
4. Keep public APIs close to the old generated APIs.
5. Update templates/generator so the generated files are reproducible.
6. Build and fix compile errors.
7. Only then update examples if needed.

---

## Do Not Do

Do not:

* rewrite the entire framework
* remove NAC-ABE logic
* remove certificate logic
* remove logging
* change protobuf definitions unnecessarily
* change NDN naming conventions unnecessarily
* modify unrelated files outside `CodeGenerator/`
* manually patch generated files without updating templates/generator
* replace the system with a completely different API
* delete old classes unless explicitly requested

---

## Build Commands

From the project root:

```bash
./waf configure
sudo ./waf build
sudo ./waf install
```

If only testing generated examples, also inspect:

```text
examples/wscript
```

---

## Logging

The project uses NDN logging.

To enable trace logging:

```bash
export NDN_LOG="*=TRACE"
```

Run an application and save logs:

```bash
./app > filename.log 2>&1
```

MiniNDN logs are usually under:

```text
/tmp/minindn/<nodeName>
```

---

## Expected Agent Workflow

Before modifying code, first explain:

1. Where old provider/user functionality currently comes from.
2. Which methods or fields are inherited from old framework base classes.
3. Which parts must be copied or reimplemented into `Generated/User.hpp` and `Generated/Provider.hpp`.
4. Which files in `Template/` or `NDNSFCodeGenerator.py` need changes.

Then make small, focused changes.

After modification, report:

1. What files were changed.
2. Whether generated files are reproducible.
3. Whether build succeeds.
4. Any remaining compatibility risks.

---

## Final Objective

After this task, application code should be able to use:

```text
CodeGenerator/Generated/User.hpp
CodeGenerator/Generated/Provider.hpp
```

directly, without relying on generated classes such as:

```text
ServiceUser_Drone.hpp
ServiceProvider_Drone.hpp
```

and without requiring inheritance from old `NDN_SERVICE_FRAMEWORK` base classes.

```
```
