# Native distributed execution roadmap

Native distributed execution is a separate roadmap from the main native language release. Local CPU tasks and Apple Metal GPU execution remain in the main roadmap; [NVIDIA/CUDA](native-cuda-roadmap.md) has its own separate roadmap. The JavaScript prototype's network runtime is historical evidence, not a completed native implementation.

The agreed design discussion must happen before native distributed execution is implemented.

## Milestones

1. **Execution and failure contract.** Decide how services are declared and deployed, what a remote invocation means, and how errors, deadlines, cancellation and disconnected clients behave. Specify retry and idempotency rules rather than assuming a local task can simply cross a network.
2. **Typed contracts and values.** Define versioned service interfaces, compatible data schemas, validation, limits and serialization. Transfer values or explicitly defined remote resources; never treat local pointers or reference counts as distributed ownership.
3. **Transport and security.** Select a transport, authentication and authorization model, encryption/deployment boundaries, message-size limits and compatibility rules.
4. **Runtime and data lifetimes.** Implement bounded admission, backpressure, completion acknowledgement, cancellation settlement and storage ownership on each host. Define CPU/GPU data movement and data locality explicitly.
5. **Deployment and observability.** Provide service configuration, discovery or explicit endpoints, diagnostics, resource limits and operational visibility.
6. **Qualification.** Verify independent processes and machines, protocol mismatches, network interruption, failure during transfer, cancellation races, cleanup, overload and recovery through complete applications.

The existing typed values, structured local tasks and backend separation can be reused where their contracts apply. Network failure and deployment require additional semantics and validation. Those concerns do not block completion of the [main native roadmap](roadmap.md#native-implementation-status).
