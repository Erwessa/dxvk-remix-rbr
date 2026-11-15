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

- Acquire predicted poses and query per-eye transforms, projections and recommended sizes.
- Add a tested conversion from OpenVR matrices to the runtime's camera convention.
- Log headset/runtime availability and actionable initialization errors.

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

## Current implementation note

The existing OpenVR bootstrap typedef for `VR_InitInternal` must match its exported ABI. This branch corrects its return type to `uint32_t`; the function's return value is the OpenVR token, while initialization success is reported through `EVRInitError`.
