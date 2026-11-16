# OpenVR stereo rendering design

## Scope

Implement a native OpenVR presentation path for the DXVK Remix runtime. The target is true stereo: the scene is rendered independently for the left and right eye from the corresponding OpenVR eye poses and projection matrices. A mono image must never be duplicated or shifted to simulate stereo.

The existing D3D9 presentation path remains the default when VR is disabled or unavailable. OpenVR is an optional runtime dependency and must not make ordinary D3D9 startup depend on SteamVR.

## Existing integration

`VrInstance` in `dxvk_openvr.cpp` currently acts as a Vulkan extension provider. It loads the OpenVR client library, obtains `IVRCompositor`, asks it for required Vulkan instance/device extensions, and shuts down after device-extension discovery. It does not own a frame loop, query tracked poses, create eye render targets, or submit eye textures.

The runtime's Vulkan presentation path is separate from the RTX renderer. `DxvkDevice::presentImage` queues a normal swapchain present through `DxvkSubmissionQueue` and `vk::Presenter`. OpenVR eye submissions therefore need an explicit GPU synchronization and ownership design; they must not be inserted as an unsynchronized call on an arbitrary application thread.

## RTX frame lifecycle constraint

`RtxContext::injectRTX` is a complete frame pipeline, not a single view render. It drains retained scene commands, validates the camera, updates per-frame state, prepares scene data/TLAS and resources, generates ray-tracing arguments, dispatches volumetrics and path tracing, performs denoising and composition, runs upscaling/post-processing, updates frame-end state and temporal history, and applies pending options.

Calling `injectRTX` twice for one game frame is not a valid stereo implementation. Its frame guard (`m_frameLastInjected`), scene preparation, resource queues, camera history, frame-end callbacks, and temporal systems are all frame-scoped. A correct stereo implementation must separate once-per-game-frame work from per-eye view work.

## Required frame model

1. Acquire one predicted OpenVR tracking sample for the game frame using the compositor's pose-wait path. Keep the predicted head pose and both eye-to-head transforms associated with the same frame.
2. Query each eye's recommended render dimensions and projection matrix. Do not assume symmetric FOV, identical per-eye projections, or a fixed headset resolution.
3. Compose the head pose with each eye-to-head transform to obtain two world-space eye poses. Convert the poses into the runtime's camera convention in one well-defined transform helper; test handedness and matrix layout explicitly.
4. Prepare scene state once for the game frame: retained-scene commands, geometry/material updates, acceleration structures, light uploads and other data that is shared by both eyes.
5. Render the left and right views independently using their own view and projection matrices. Both views must use the same scene snapshot and frame tracking sample.
6. Preserve independent temporal histories for the two eyes. Histories, jitter, motion vectors, depth, denoiser inputs, and upscaler state must not be accidentally shared between eyes unless a subsystem is explicitly designed for multiview operation.
7. Submit the two completed Vulkan images to `IVRCompositor::Submit` with the correct eye identifiers, Vulkan texture metadata, image layout, queue/device information, and synchronization. Handle compositor errors and device loss without wedging the normal present path.
8. Complete once-per-game-frame cleanup and advance Remix frame state exactly once.

## Render-target and synchronization requirements

- Use the compositor's recommended per-eye dimensions as the initial target size. Recreate eye resources only when dimensions or relevant formats change.
- Use Vulkan images with usage flags compatible with the actual RTX render and OpenVR compositor access paths.
- Transition each submitted image to the layout required by OpenVR's Vulkan texture contract before submission, and do not reuse it until the compositor/runtime has released it according to the synchronization contract.
- Serialize access to the Vulkan queue shared with OpenVR. Respect the runtime's queue-family and device requirements; do not assume the game's present queue is automatically valid.
- Keep resource lifetime, semaphores/fences, image layout tracking, and queue ownership explicit. A CPU-side `WaitGetPoses` call is not a substitute for GPU completion synchronization.

## Matrix convention audit

The checked-in DXVK math type `Matrix4Base` stores four columns and multiplies column vectors: `(A * B) * v` applies `B` first, and the translation of an affine transform is in column 3. This is the same convention as `OpenVrMatrix4`, so the conversion changes only the storage order, not the math: `Matrix4[column][row] = OpenVrMatrix4.at(row, column)`. Do not transpose the 3-by-3 rotation. `matrixFromRowMajor()` in `rtx_vr_math.h` does this conversion. See `documentation/VrMatrixConventions.md`.

OpenVR's documented tracking basis is +X right, +Y up, -Z forward, with positions in metres. This establishes the source convention, but not the final game-world mapping. Before constructing an HMD camera, the implementation must still define the tracking origin/recenter policy, map metres to the RBR world-unit scale, and compose the head pose with the game's base camera without double-applying its orientation. Projection conversion is a separate operation: raw asymmetric bounds must be converted to the exact clip-space/depth convention expected by Remix's RTX passes.

No OpenVR-to-DXVK conversion helper is implemented yet. The above is the matrix-layout finding only; it is not a claim that the camera transform is already correct.

## Camera and temporal-history requirements

The existing `CameraType` values describe scene roles such as Main, ViewModel, Sky, Portal and RenderToTexture; they are not left/right eye slots. Stereo eye state should be represented explicitly rather than overloading those scene camera roles.

The Main camera is consumed by many RTX passes, and `RtCamera` stores previous-frame transforms, projection matrices, jitter and frustum data. The stereo implementation must establish how those values are indexed by eye before changing camera-manager storage. Reusing one Main camera sequentially for both eyes would overwrite temporal state and produce incorrect motion/history data.

## Staged implementation

### Stage A — OpenVR runtime ownership

- Separate extension discovery/bootstrap from the lifetime of the active VR session.
- Resolve and validate the OpenVR interfaces needed by the renderer.
- Initialize as an appropriate scene application only when VR mode is explicitly enabled.
- Add deterministic shutdown and failure cleanup; preserve the non-VR path.

### Stage B — tracking and calibration

- [x] Add guarded access to `WaitGetPoses` for the active scene session.
- [x] Query the recommended render-target size, per-eye eye-to-head transform, and raw asymmetric projection bounds.
- [x] Add a conversion from OpenVR matrices and raw projection bounds to the runtime's camera convention (`rtx_vr_math.h`). It was checked numerically outside the build, and the vertical sign of the raw bounds still has to be confirmed on a headset.
- [ ] Log headset/runtime availability and actionable initialization errors.

The pose and calibration methods are currently runtime primitives only. No frame loop consumes them yet, and no camera matrices are changed by this stage.

### Stage C — per-eye renderer refactor

- Split once-per-frame scene preparation and cleanup from per-view rendering.
- Introduce explicit eye-indexed camera state and temporal resources.
- Render both eyes from one prepared scene snapshot.
- Keep existing mono D3D9 rendering behavior unchanged when VR is off.

### Stage D — compositor submission

- Create and manage per-eye Vulkan output images.
- Add explicit queue synchronization, image-layout transitions and OpenVR texture submission.
- Handle compositor submit errors and orderly resource teardown.

### Stage E — validation

- Build the runtime using `scripts/build.ps1`.
- Validate that non-VR D3D9 startup and presentation are unchanged.
- Validate that both eyes receive independently rendered images and correct asymmetric projections.
- Check eye alignment, scale, world stability during head rotation/translation, frame pacing, GPU synchronization, and repeated enable/disable cycles.
- Test with Vulkan validation enabled and inspect logs for layout, lifetime and queue-synchronization errors.

## RTX frame and camera findings

The current `RtxContext::injectRTX()` is a frame-level transaction, not a reusable eye-render function. It drains scene commands, commits graphics state, refreshes shader prewarming, applies the fallback Main camera, sets present throttling, gates duplicate injection with `m_frameLastInjected`, submits particle and texture work, and then records the RTX frame graph. Calling it twice in one device frame returns at the duplicate-injection guard and, even without that guard, would repeat frame-scoped work.

`CameraManager` owns a fixed array of `RtCamera` objects. Each `RtCamera` contains current and previous matrix caches, Halton jitter state, camera-cut/history invalidation state, and frame-touch metadata. `CameraManager::onFrameEnd()` resets the last-set camera marker and the projection decomposition cache. This state cannot be shared between eyes without defining which temporal quantities are frame-scoped and which are view-scoped.

### Required refactor boundary

1. Keep scene command draining, resource uploads, shader readiness checks, acceleration-structure updates, and other scene preparation once per game frame.
2. Introduce an explicit view description for each eye: predicted HMD pose composed with eye-to-head transform, asymmetric projection, render extent, and eye identity.
3. Run the view-dependent RTX passes for each eye with separate camera matrices and temporal state. The left eye's history must never become the right eye's previous-frame history.
4. Keep frame-level counters, Reflex bookkeeping, particle submission, capture triggers, and presentation bookkeeping single-shot unless a subsystem is explicitly made view-aware.
5. Submit both eye images only after their Vulkan work is complete and the images are in the layout required by OpenVR. Serialize access to the Vulkan queue across OpenVR calls that may use it.

Do not implement this by calling `injectRTX()` twice or by changing `m_frameLastInjected` to permit duplicate injection. The correct extraction point is after the shared scene preparation and before the view-dependent render graph is recorded. The exact boundary must be confirmed against the remainder of `injectRTX()` and the resources it mutates before code is moved.

## Detailed `injectRTX()` pass classification

The full current implementation was reviewed from the entry point through `onInjectRtxFrameEnd()`. The following is the proposed split, not a claim that these operations are already stereo-safe.

### Frame-scoped work (one execution per game frame)

- Drain scene commands, imported-image barriers, and commit pending graphics state.
- Refresh shader prewarming, evaluate ray-tracing support/options, set present throttle, and perform the duplicate-injection guard.
- Process hot reloads, global performance/session logging, screenshot/capture triggers, and deferred retained-geometry replay.
- Submit particle draw state and texture uploads; signal Reflex frame bookkeeping.
- Begin pre-exposure, latch frame lighting conditions, update pipeline cache/initializer state.
- `SceneManager::prepareSceneData()`, including the scene GPU buffers and acceleration structures.
- Cone-map generation for materials registered during this frame.
- Frame-level feature mask / counters / metrics, capture state, settings sweep, option application, and frame-end bookkeeping.

These operations must not be repeated for the second eye. Some may later need explicit per-view inputs (for example, camera-dependent scene data), but their lifetime and mutation must remain frame-scoped.

### View-scoped work (one execution per eye, after refactor)

- `onInjectRtxFrameBegin()` and render extent selection, once its global/resource side effects are separated from view-specific sizing.
- Ray-tracing arguments that encode view/projection dimensions and camera data.
- Camera-dependent passes: path tracing/G-buffer, screen-space cloud post-depth, view-dependent RTXDI/ReSTIR queries, demodulation, denoising, composition, and debug replacement.
- Per-view upscaling/reconstruction and post-processing where the algorithm consumes view history or camera motion.
- Final per-eye output image generation.

A number of these passes use shared managers or resource queues today. They cannot simply be called twice: each such manager/resource must either be proven frame-global and read-only during view execution, or receive an explicit per-eye state/resource set.

### Special handling required before stereo

- `Resources::RaytracingOutput` currently exposes a single primary output set and advances depth/motion-vector queues at frame start. Stereo needs per-eye output/history resources or a deliberate sequential reuse strategy with fully isolated history.
- `m_currentUpscaler`, `m_previousUpscaler`, and DLSS/RR/XeSS/FSR/TAA state are currently context-level. Their temporal state must be separated by eye, and vendor SDK integration must be checked for multi-view support and correct motion/depth inputs.
- `PreExposure::beginFrame()`, `LightBvh::latchFrame()`, Reflex, dust simulation, particle submission, sampler feedback, object picking, thumbnails, captures, performance windows, and settings sweeps are not automatically safe to execute twice.
- `dispatchFrameInterpolation()` and DLFG require a separate decision: frame generation cannot be assumed to accept two independent eye streams without a dedicated stereo path.
- The game target blit and screenshot/thumbnail capture currently describe a single image. VR output must be a separate pair of eye images and must not accidentally overwrite or capture one eye as the game's mono target.

### Device-scoped integration decision

Do not start a scene session unconditionally from `DxvkDevice` construction yet. The current `VrInstance` is a process-wide singleton, while DXVK devices are individually constructed and destroyed. A constructor/destructor pair on every device could cause duplicate initialization, premature shutdown, or a mismatch between the Vulkan device used in `VRVulkanTextureData_t` and the device that owns the scene session.

Before wiring lifecycle calls, establish these invariants:
1. There is one explicitly selected DXVK device that owns OpenVR rendering for the process.
2. VR mode is explicitly enabled by the host/configuration; merely discovering OpenVR Vulkan extensions must not silently turn on HMD rendering.
3. The owning device is the same device whose logical device, physical device, instance, graphics queue, and family are placed in each OpenVR Vulkan texture descriptor.
4. Device teardown first stops new frame acquisition/submission, waits for all submitted eye images to become reusable, then releases eye resources, and only then shuts down a DXVK-owned OpenVR scene session.
5. If the runtime session is host-owned, DXVK releases only its own per-device submission state and never invokes global `VR_ShutdownInternal`.

For the first implementation, support one OpenVR-owning DXVK device and fail closed if a second device attempts to claim the process-wide session. Add multi-device/ref-counted ownership only if an actual target application requires it. This is safer than adding implicit reference counting around a runtime API whose initialization ownership is process-global.

## Vulkan texture descriptor mapping

The checked-in OpenVR SDK header `include/openvr/openvr.hpp` defines `vr::VRVulkanTextureData_t` with these fields:
- `m_nImage`: Vulkan image handle encoded as `uint64_t`.
- `m_pDevice`: logical device.
- `m_pPhysicalDevice`: physical device.
- `m_pInstance`: Vulkan instance.
- `m_pQueue` and `m_nQueueFamilyIndex`: queue and its family.
- `m_nWidth`, `m_nHeight`, `m_nFormat`, `m_nSampleCount`: image metadata.

The current DXVK object graph already exposes the matching values:
- `DxvkDevice::handle()` and `DxvkDevice::vkd()` expose the logical device / dispatch table.
- `DxvkDevice::adapter()->handle()` exposes the physical device.
- `DxvkDevice::instance()->handle()` exposes the Vulkan instance.
- `DxvkDevice::queues().graphics.queueHandle` and `queueFamily` expose the graphics queue and family.
- The exact eye image provides the remaining image handle, dimensions, format, and sample count.

This is a strong indication that OpenVR Vulkan submission can use the existing DXVK device and graphics queue; no second Vulkan device or instance is needed. The mapping must still be implemented at a point where the image and device are guaranteed to outlive the compositor's use.

### Ownership and lifetime warning

The SDK header explicitly says to call OpenVR shutdown before destroying Vulkan resources passed through this structure. Therefore:
- Eye images must be destroyed only after compositor use is complete.
- Scene-session shutdown must precede destruction of any submitted eye images and the Vulkan device.
- If the scene session is host-owned, DXVK must not call global OpenVR shutdown; it still must stop submitting its own images and wait for compositor use to finish before releasing those images.
- The current singleton `VrInstance::s_instance` means session ownership is process-wide, whereas `DxvkDevice` is device-scoped. A future integration must handle multiple DXVK devices or repeated device creation explicitly rather than blindly shutting down the singleton from every device destructor.


## OpenVR submission-image compatibility audit

There is a useful positive finding in the existing allocator:
- `Resources::createImageResource()` adds `VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT` to every image it creates, plus any requested extra usage flags.
- The current final RTX output is an `AliasedResource` with `VK_FORMAT_R16G16B16A16_SFLOAT`; the allocator's default usage flags therefore include the two usage bits required by OpenVR, and this format is in Valve's documented Vulkan compositor format list.
- This makes the existing image allocator a plausible basis for compositor-compatible eye targets, subject to confirming the actual image/view passed to OpenVR and its final layout.

Important remaining mismatch: `createTargetResources()` sizes `m_finalOutput` from the single `m_targetExtent`, which is currently driven by the game's target resolution. OpenVR's recommended eye render size is queried by the new wrapper but is not yet connected to resource allocation. Do not assume the game backbuffer extent is the HMD's recommended per-eye extent. A future VR target path must explicitly choose and track the per-eye extent, preserve the desktop target separately if needed, and recreate per-eye resources when either extent changes.

The usage flags alone are insufficient: before `IVRCompositor::Submit`, the exact image must have completed rendering, be transitioned to `VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL`, and have correct queue-family ownership and synchronization. The image must remain alive until the compositor is finished with it.

## Vulkan queue synchronization contract

The repository already has the synchronization primitive needed for external Vulkan queue users; a new ad-hoc mutex must not be introduced.

- `DxvkDevice::lockSubmission()` first calls `m_submissionQueue.synchronize()` and then locks the queue mutex. The current `synchronize()` implementation waits until the CPU-side submit queue is empty and synchronizes the last presenter when present; it is not a general `vkDeviceWaitIdle` and must not be described as such.
- `DxvkDevice::lockSubmissionUnsynchronized()` locks the same queue mutex without first draining queued submissions. Its header explicitly warns that mixing queued DXVK work and direct Vulkan submissions this way can break ordering.
- `DxvkDevice::unlockSubmission()` releases the queue mutex.
- The submission thread holds that same mutex while it submits command lists. A command list can submit both the dedicated transfer queue and graphics queue while under this gate.


The device exposes these methods publicly in `dxvk_device.h`, so the intended integration point exists without exposing `m_mutexQueue` or adding a second lock. The queue mutex is held by the submission thread around `DxvkCommandList::submit()`; that submit can target the dedicated transfer queue and then the graphics queue. Keep the OpenVR Vulkan queue description aligned with the queue actually passed to DXVK's graphics submission path, and verify that the chosen queue family supports the required operations.

The Valve OpenVR Vulkan contract adds several constraints (see the official [OpenVR Vulkan documentation](https://github.com/ValveSoftware/openvr/wiki/Vulkan)):
- The runtime can access the queue described by `VRVulkanTextureData_t` during `IVRCompositor::Submit`; the application must not access that queue concurrently until the call returns.
- `WaitGetPoses` may also access the queue in the default timing mode. `PostPresentHandoff` and `SubmitExplicitTimingData` access it as well.
- Submitted images must be in `VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL`, and require at least `VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT`.

Proposed initial synchronization policy:
1. Route every OpenVR call that may touch the Vulkan queue through the existing `DxvkDevice` submission gate.
2. Use `lockSubmission()` for the first conservative implementation where ordering against already queued DXVK work matters; pair it with `unlockSubmission()` using an exception-/early-return-safe scope guard.
3. Submit both eyes under one queue-lock interval if the render path has already transitioned both images and established their producer dependencies. Do not release the lock between calls if doing so would permit another thread to enter the queue between the two compositor submissions.
4. Do not use `lockSubmissionUnsynchronized()` until the frame scheduler explicitly proves the required order between queued DXVK command lists and runtime queue work.
5. Never call `WaitGetPoses`, `Submit`, `PostPresentHandoff`, or `SubmitExplicitTimingData` while another thread may concurrently submit/present on the same Vulkan queue.
6. Verify whether the selected OpenVR timing mode makes `WaitGetPoses` queue access avoidable before attempting to overlap pose wait and GPU submission. Explicit timing is not a free optimization: `SubmitExplicitTimingData` itself accesses the queue and must be serialized.

This audit identifies an existing safe locking boundary, but it does not yet prove that the current `DxvkDevice` API exposes all Vulkan handles required by `VRVulkanTextureData_t`, nor that the OpenVR runtime's returned queue work is represented by DXVK's own completion tracking. Those must be addressed before implementing image submission or resource destruction.

## Runtime lifecycle integration status

The OpenVR primitives are currently not connected to a device/frame lifecycle:
- `DxvkInstance::DxvkInstance()` registers `VrInstance::s_instance` as an extension provider.
- Extension discovery can acquire the compositor using a background application session, then `initDeviceExtensions()` calls `shutdown()` after querying the Vulkan device extensions.
- `initializeSceneSession()`, `waitGetPoses()`, the recommended-size query, eye transforms, and projection queries currently have no call sites outside their declarations/definitions.
- `DxvkDevice` construction obtains Vulkan queues and initializes device-owned objects; its destructor waits for device idle before teardown. No OpenVR scene-session start/stop is currently tied to that device lifecycle.

Therefore the existing code provides OpenVR extension discovery and callable primitives, but does not yet start a persistent scene session or drive an HMD frame. The next integration step must define an explicit ownership/lifecycle policy:
1. Start the scene session only after the Vulkan device and its queue handles exist.
2. Decide whether the feature is opt-in (recommended initially) and how the RBR host requests VR mode.
3. Ensure only the code that initialized the OpenVR runtime calls `VR_ShutdownInternal`; a reused host-owned session must remain host-owned.
4. Stop the scene session before device teardown, after any outstanding compositor submissions have completed.
5. Keep `WaitGetPoses` on the designated render thread and call it once per HMD frame, not once per eye.
6. Do not assume the current queue mutex protects OpenVR compositor access to the Vulkan queue; queue serialization must be coordinated with DXVK's actual queue synchronization.

The extension-provider initialization path is not itself the VR runtime frame loop and must not be used as a substitute for scene-session ownership.

## Camera and resource ownership audit

### Camera update is frame-idempotent

`RtCamera::update()` returns immediately when `m_frameLastTouched == frameIdx`. A second update of the same camera for the other eye in the same game frame is therefore ignored. The object also owns mutable previous/current matrix caches, jitter sequence state, resolution state, and camera-history invalidation metadata.

`CameraManager` stores a fixed `std::array<RtCamera, CameraType::Count>`. Its constructor initializes every entry by iterating the enum, while `accessCamera()` asserts that the enum value is below `CameraType::Count`; `Unknown` aliases `Main`. The manager additionally owns frame-global last-camera tracking, projection decomposition cache, fallback-camera state, external world origins, and camera-cut/history invalidation markers.

Consequences:
- Never update `Main` twice to represent left and right eyes in one frame.
- Do not append eye values to `CameraType` until all enum consumers, loops, serialization/debug paths, and special-case `Main` behavior have been audited.
- Each eye needs an independently updated camera object and independent temporal/jitter history. A dedicated VR view-camera owner is a safer initial boundary than overloading `m_lastSetCameraType` or `Unknown`.
- Shared scene cameras (sky, portals, view model, render-to-texture) must retain their current semantics; an HMD eye is a render view, not another game draw camera category.

### Resource ownership is broader than depth and motion vectors

`Resources::RaytracingOutput` contains many single-instance primary-view resources beyond the two queued images: normals, albedo, linear view-Z, virtual motion vectors, hit distance, view direction, surface flags, RTXDI resources, radiance, denoising intermediates, composite/final outputs, and post-FX intermediates. It also contains shared resources explicitly written by both primary and secondary (PSR) passes.

`Resources::createRaytracingOutput()` tracks one downscaled extent and one target extent. A resize waits for device idle and rebuilds that single resource set. `createDlfgResourceQueues()` allocates one depth/motion queue, with additional entries only when DLFG/FSR frame generation is enabled; the queue is advanced once in the current injection path.

This means stereo cannot be made correct by duplicating only the depth and motion-vector queues. A resource-by-resource read/write audit is required to classify:
1. frame-shared resources (written once and safely read by both eyes),
2. per-eye render targets and temporal histories,
3. resources that can be reused sequentially only after the first eye has completed all consumers,
4. resources requiring SDK-specific multi-view handling.

The initial stereo implementation should prefer explicit per-eye resource bundles for all view-dependent render targets and histories. Sharing should be opt-in and justified by pass access analysis, not assumed from a resource's current name or allocation site.

## Recommended extraction sequence

1. Extract a single-shot `beginRtxFrame()` / `prepareRtxFrame()` boundary while preserving the existing mono behavior exactly.
2. Introduce an explicit `RtxView` description (eye index, predicted pose, eye-to-head transform, view/projection matrices, output extent).
3. Extract `renderRtxView(view, viewResources)` from the camera-dependent section. Keep the mono caller using one view so regressions can be bisected.
4. Move camera history and view-dependent resource queues into per-eye state.
5. Add two OpenVR output images and submit them only after their rendering commands and image-layout transitions are complete.
6. Validate mono output equivalence, then stereo geometry, then temporal stability and performance.

Do not move a pass until its reads/writes, manager state, history ownership, and synchronization have been traced. The mono path remains the reference and must remain unchanged when VR is disabled.

## Current implementation note

The existing OpenVR bootstrap typedef for `VR_InitInternal` must match its exported ABI. This branch corrects its return type to `uint32_t`; the function's return value is the OpenVR token, while initialization success is reported through `EVRInitError`.


## Atomic OpenVR frame snapshot

`VrInstance::acquireFrame(OpenVrFrameState&)` now gathers one frame as a single operation under the OpenVR instance mutex:
- Calls `IVRCompositor::WaitGetPoses` exactly once.
- Copies the full render-pose array, including the HMD pose.
- Captures both eye-to-head transforms and each eye's raw projection bounds.
- Increments a monotonically increasing DXVK-side frame index after a successful compositor wait.
- Publishes the snapshot only after all fields have been gathered; returns false when the HMD pose is invalid.

The eye transforms are intentionally documented as eye-to-head transforms. The eventual camera integration must invert them when composing world-to-eye view transforms. This snapshot layer does not yet convert OpenVR matrices to DXVK/GLM conventions, choose a tracking origin, render either eye, or submit textures. Those steps require the camera and RTX view-matrix conventions to be audited first.

The caller must still hold DXVK's submission queue gate around `acquireFrame`, because OpenVR's compositor wait may access the Vulkan queue. That synchronization belongs at the future frame-loop call site, not inside the global runtime singleton.


## OpenVR tracking-space view matrices and exclusive session ownership

The frame snapshot now also carries:
- The HMD's `trackingFromHead` rigid transform from the compositor pose.
- `eyeFromTracking[2]`, composed as `inverse(headFromEye) * inverse(trackingFromHead)`.
- OpenVR's recommended per-eye render-target dimensions.

These matrices are row-major and remain entirely in OpenVR tracking coordinates. They do not apply an RBR world origin, meters-to-world scale, or DXVK/GLM clip-space conversion. Those are deliberately separate integration steps; the raw projection bounds remain available for constructing the renderer-specific asymmetric projection matrix.

The session API now requires an explicit owner token. Only the same owner may re-enter an active session or release it; a second DXVK device is rejected instead of accidentally sharing a process-global runtime session or shutting it down prematurely. The token is expected to be the address of the selected owning `DxvkDevice`. Runtime ownership remains separate: a host-owned OpenVR runtime is reused and is never globally shut down by DXVK.


## DXVK device lifecycle hookup

The selected DXVK device now attempts to claim the OpenVR scene session after its Vulkan queues and device-owned infrastructure are initialized. The claim is opt-in through the `rtx.vr.enable` option (default off, read once at device creation); `DXVK_NO_VR=1` remains a hard disable. If the runtime is unavailable, the claim fails and the existing mono path continues unchanged. A second DXVK device cannot claim the same process-global scene session.

On teardown, the device first drains its DXVK work with the existing `waitForIdle()` path, then releases its own OpenVR claim before device-owned Vulkan state is destroyed. The session owner token prevents a non-owner device from shutting down the active session. This is lifecycle plumbing only: no frame acquisition or eye-image submission is wired into the game frame loop yet.


## Dedicated RTX camera state per HMD eye

`CameraManager` now owns two independent `RtCamera` instances exposed through `getVrEyeCamera(RtxEye::Left/Right)`. They are deliberately separate from `CameraType`: Main/ViewModel/Sky/Portal/RenderToTexture remain scene roles, while left/right are render views. Each eye therefore has its own frame-id guard, current/previous matrices, jitter sequence, and view-history invalidation state. The ordinary mono camera array and its classification logic are unchanged.

These camera slots are storage and access infrastructure only. The frame loop does not yet update them or feed them into the RTX passes; that requires the RBR-to-OpenVR tracking calibration and the view-scoped RTX pipeline split described above.


## Explicit game-world calibration contract

The runtime now exposes an explicit `OpenVrWorldCalibration` input and a pure
`OpenVrFrameState::getEyeFromGameWorld()` composition helper. The calibration
matrix is named by its mapping direction: `trackingFromGameWorld` maps an RBR
game-world position into OpenVR tracking coordinates. Its affine transform must
encode the selected recenter/tracking origin, axis and handedness conversion,
and game-units-to-metres scale. It is invalid until the RBR integration supplies
all of those values; there is no guessed identity/default calibration.

For a valid HMD frame, the helper computes:

```
eyeFromGameWorld = eyeFromTracking[eye] * trackingFromGameWorld
```

This helper uses column-vector composition. DXVK's `Matrix4` uses the same
convention, so the completed matrix only changes storage order at the boundary
(`matrixFromRowMajor()`) before it is passed to `RtCamera`.
The helper does not update a camera, modify a game camera, or imply that
calibration has been discovered automatically. This keeps the tracking-space
math testable and prevents an uncalibrated headset pose from moving the scene.


## RBR/D3D9 camera-source audit

The target game's ordinary camera reaches Remix through the D3D9 transform state,
not through a game-specific physics-camera API:

1. `D3D9DeviceEx::SetTransform(D3DTS_VIEW, ...)` validates the transform and
   routes it to `SetStateTransform`.
2. `SetStateTransform` converts and stores the matrix in
   `m_state.transforms[GetTransformIndex(D3DTS_VIEW)]`, then marks the
   fixed-function vertex data dirty.
3. The D3D9 draw/frontend path builds its camera data from that stored View and
   Projection state. `CameraManager::processCameraData()` ultimately calls
   `applyFrameCamera(Main, worldToView, viewToProjection)`.
4. `RtCamera::update()` stores the supplied world-to-view matrix and its
   inverse. `CameraManager::getMainCamera()` therefore exposes the renderer's
   already accepted main camera, including the current `getViewToWorld()`
   transform when the camera is valid for the current frame.

This gives the VR path a stable integration source: use the current Remix Main
camera as the game-world reference, rather than reconstructing the car or camera
from RBR gameplay/physics state. The two eye cameras can be derived from this
reference and one predicted OpenVR tracking snapshot.

### World origin and units are not yet established

The existing `rtx.worldOrigin` support is an opt-in host-published render-origin
mechanism. It reads `__mcrtx.world_origin.x/y/z` game values and documents the
convention `render position + world origin = world position`. This is not
evidence that RBR currently publishes those values, and it is not an automatic
game-unit conversion.

The D3D9 transform path proves where the view matrix comes from, but it does not
prove what one RBR world unit represents in metres. Nor does it establish a
fixed world-axis mapping independently of the actual camera matrices. The
current source audit therefore confirms the camera source and renderer matrix
convention, but intentionally leaves the metres-per-game-unit value and
tracking-to-game axis calibration unset.

### Camera validity and frame timing

`CameraManager::getMainCamera()` is the relevant camera accessor, and
`RtCamera::isValid(frameId)` / `getLastUpdateFrame()` expose whether it has
been updated for the current DXVK frame. The camera manager also has dedicated
left/right camera objects, but they are not currently populated from the main
camera or the HMD.

A future frame coordinator must not sample the main camera too early: D3D9
updates it while processing draws, and a frame may contain menu, sky,
view-model, and render-to-texture cameras before or after the main camera.
Use the Main camera only after its current-frame validity has been established;
do not mistake `getLastSetCameraType()` for a persistent camera identity or
assume the last draw camera is always Main.

The first safe integration should therefore have two explicit phases:
- Capture/retain the accepted Main camera snapshot for the game frame.
- Once the frame's camera and OpenVR predicted pose are both available, derive
  left/right view matrices from the same base camera and the same tracking
  sample, then update each eye camera exactly once.

The D3D9 Main camera is a view/projection input to Remix, not a declaration of
the physical HMD tracking origin. A recenter/anchor policy and a calibrated
uniform scale must still be supplied before any headset translation is applied
to the game world.


### D3D9 draw-time camera capture and threading

The D3D9 v2 frontend has an application-thread `CameraClassifier` that mirrors
the render-thread `CameraManager::processCameraData()` classification. Its
commented contract is explicit: draw order is the same on both threads, and
the first Main classification of a D3D9 frame is the point at which the Main
camera is established. The classifier retains per-type view matrices for
frontend-side decisions; it is not the authoritative RTX camera object.

On the render thread, `RtxContext::prepareGeometryForRTImpl()` first applies
any pending host world-origin update, then calls
`cameraManager.processCameraData(drawCallState)`. That call selects a camera
type and updates its `RtCamera` from the draw's `worldToView` and
`viewToProjection` matrices. This means the authoritative Main camera becomes
available during geometry preparation, not necessarily at the beginning of
the host's frame and not at the first D3D9 API call that sets D3DTS_VIEW.

Implications for the future VR coordinator:

- Do not call `WaitGetPoses` from `SetTransform` or from the app-thread
  `CameraClassifier`; those are not the designated compositor/frame boundary.
- A frame-start pose sample can be retained while waiting for the first valid
  Main camera, but it must be paired with the correct game-frame identifier.
- Alternatively, capture the current Main camera once it becomes valid and
  consume the already-acquired predicted pose for that same frame.
- A Main camera may be absent or rejected on menus/UI-only frames. Such a frame
  must not update eye histories from stale camera data or submit a fabricated
  stereo scene.
- Sky and view-model cameras can be observed later in draw order. The eye
  camera source must remain explicitly `CameraType::Main`, not whichever
  camera was most recently set.

This confirms the capture point and thread ownership, but does not yet provide
a safe place to run the two-eye render graph. That remains a separate frame
scheduler/refactor task.
