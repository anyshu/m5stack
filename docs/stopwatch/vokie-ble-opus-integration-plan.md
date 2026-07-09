# M5Stack StopWatch BLE Opus Integration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build M5Stack StopWatch as a Vokie BLE external microphone and controller, using Opus 16kbps audio over BLE and feeding decoded PCM into the existing Vokie ASR pipeline.

**Architecture:** StopWatch integration is isolated behind BLE, protocol, jitter buffer, Opus decoder, audio source, and presenter modules. Vokie stores StopWatch as a virtual microphone id, routes audio capture to `StopWatchBleAudioSource`, and keeps ASR/post-process/history/paste logic unchanged.

**Tech Stack:** Electron main process, Vue renderer, TypeScript, BLE GATT, Opus decoder, existing Vokie `ConfigPresenter`, `VoiceSessionCoordinator`, capture pipeline, Vitest, `pnpm typecheck`.

## Global Constraints

- First version uses BLE only for control, status, and Opus audio; no Wi-Fi path.
- Audio transport format is Opus at 16000 bps, 16000 Hz, mono, 20 ms frames.
- Vokie decoder output is PCM s16le 16k mono.
- Selecting StopWatch sets `defaultMicrophoneId` to `vokie-stopwatch:<deviceId>` and `defaultMicrophoneLabel` to `M5Stack StopWatch`.
- StopWatch events use semantic button events, not simulated keyboard shortcuts.
- Existing ASR, post-process, paste, history, and learning-loop pipelines must be reused.
- First version returns final ASR text to StopWatch; partial ASR text is not implemented.
- USB is for development and diagnostics only.
- This work does not include a version number update.
- Commit messages must be Chinese if the user explicitly requests commits.
- Do not commit or push unless the user explicitly asks.
- Only stage/commit files modified for this feature.

---

## File Structure

- Create `src/shared/stopwatch.ts`
  - Shared constants, GATT UUIDs, virtual microphone id helpers, event and state types.
- Create `src/main/services/stopwatch/protocol.ts`
  - Encode/decode Control JSON, Audio binary frames, Status JSON, Config payloads.
- Create `src/main/services/stopwatch/jitterBuffer.ts`
  - Keep audio frames ordered by `seq`, drop duplicates, detect gaps.
- Create `src/main/services/stopwatch/opusDecoder.ts`
  - Wrap the selected Opus decoder implementation and expose PCM `Int16Array` output.
- Create `src/main/services/stopwatch/stopwatchBleService.ts`
  - Manage BLE device connection, notifications, writes, battery/device state.
- Create `src/main/services/stopwatch/stopwatchAudioSource.ts`
  - Convert BLE Opus frames into PCM chunks consumable by the existing recording pipeline.
- Create `src/main/presenter/stopwatchDevicePresenter.ts`
  - IPC-facing presenter for scan/connect/disconnect/status/default microphone operations.
- Modify `src/main/presenter/index.ts`
  - Instantiate and register `StopwatchDevicePresenter`; route external trigger events into `VoiceSessionCoordinator`.
- Modify `src/main/presenter/voiceSessionCoordinator.ts`
  - Add external trigger method and reuse current toggle/PTT/cancel control paths.
- Modify capture/audio source selection files after locating the exact capture abstraction in `src/renderer/common/pcm/usePcmCapture.ts` and related main-process bridge points.
  - Route `vokie-stopwatch:*` microphone ids to `StopWatchBleAudioSource`.
- Modify settings UI files after locating the current microphone/settings components.
  - Add StopWatch status card and show StopWatch in microphone selection.
- Add tests next to the new modules or in the existing test layout used by nearby modules.

## Task 1: Shared StopWatch Types And ID Helpers

**Files:**
- Create: `src/shared/stopwatch.ts`
- Test: `src/shared/stopwatch.test.ts`

**Interfaces:**
- Produces:
  - `STOPWATCH_VIRTUAL_MIC_PREFIX`
  - `STOPWATCH_DEFAULT_LABEL`
  - `isStopwatchMicrophoneId(id: unknown): id is string`
  - `createStopwatchMicrophoneId(deviceId: string): string`
  - `parseStopwatchDeviceId(microphoneId: string): string | null`
  - `STOPWATCH_GATT_UUIDS`

- [ ] Write tests for virtual microphone id creation, parsing, and rejection of non-StopWatch ids.
- [ ] Implement `src/shared/stopwatch.ts` with constants and helpers.
- [ ] Run `pnpm test -- src/shared/stopwatch.test.ts`.
- [ ] Run `pnpm typecheck`.
- [ ] If the user asks for a commit, stage only these files and use a Chinese commit message.

## Task 2: Protocol Codec

**Files:**
- Create: `src/main/services/stopwatch/protocol.ts`
- Test: `src/main/services/stopwatch/protocol.test.ts`

**Interfaces:**
- Consumes: `STOPWATCH_GATT_UUIDS` and shared StopWatch types.
- Produces:
  - `parseControlMessage(input: Buffer | string): StopwatchControlMessage`
  - `encodeStatusMessage(message: StopwatchStatusMessage): Buffer`
  - `parseConfigMessage(input: Buffer | string): StopwatchConfigMessage`
  - `encodeAudioFrame(frame: StopwatchAudioFrame): Buffer`
  - `parseAudioFrame(input: Buffer): StopwatchAudioFrame`

- [ ] Write protocol tests for valid button, battery, device, config, status, and audio frames.
- [ ] Write protocol tests rejecting invalid JSON, unsupported version, bad magic, truncated audio header, and mismatched payload length.
- [ ] Implement Control JSON parsing with strict `v === 1` and finite numeric `seq`.
- [ ] Implement binary Audio frame parsing using little-endian fields and magic `0x56 0x4f`.
- [ ] Implement Status JSON encoding with final text truncation to 120 UTF-8 characters plus ellipsis.
- [ ] Run `pnpm test -- src/main/services/stopwatch/protocol.test.ts`.
- [ ] Run `pnpm typecheck`.

## Task 3: Jitter Buffer

**Files:**
- Create: `src/main/services/stopwatch/jitterBuffer.ts`
- Test: `src/main/services/stopwatch/jitterBuffer.test.ts`

**Interfaces:**
- Consumes: `StopwatchAudioFrame` from `protocol.ts`.
- Produces:
  - `class StopwatchJitterBuffer`
  - `push(frame: StopwatchAudioFrame): StopwatchJitterBufferResult`
  - `reset(streamId?: number): void`
  - result values for `ready`, `duplicate`, `gap`, `late`, and `stream-reset`.

- [ ] Write tests for sequential frames emitting immediately.
- [ ] Write tests for out-of-order frames being reordered within the configured window.
- [ ] Write tests for duplicate frames being ignored.
- [ ] Write tests for stream id change resetting state.
- [ ] Write tests for gap detection without retry.
- [ ] Implement the smallest buffer that satisfies the tests with a fixed small reorder window.
- [ ] Run `pnpm test -- src/main/services/stopwatch/jitterBuffer.test.ts`.
- [ ] Run `pnpm typecheck`.

## Task 4: Opus Decoder Wrapper

**Files:**
- Create: `src/main/services/stopwatch/opusDecoder.ts`
- Test: `src/main/services/stopwatch/opusDecoder.test.ts`
- Modify: `package.json`
- Modify: `pnpm-lock.yaml`

**Interfaces:**
- Consumes: Opus payload from `protocol.ts`.
- Produces:
  - `class StopwatchOpusDecoder`
  - `decode(payload: Buffer): Int16Array`
  - `decodeMissingFrame(): Int16Array`
  - `reset(): void`

- [ ] Select a Node/Electron-compatible Opus decoder dependency that supports 16k mono decode in the main process.
- [ ] Add dependency to `package.json` and update `pnpm-lock.yaml`.
- [ ] Add an Opus fixture generated from 20 ms of known 16k mono speech or tone.
- [ ] Write a decoder test that verifies decoded PCM length is 320 samples for a 20 ms frame.
- [ ] Write a missing-frame test that verifies PLC returns a 320-sample frame or an explicit silence frame if the dependency cannot PLC.
- [ ] Implement decoder wrapper so the rest of the app never imports the decoder dependency directly.
- [ ] Run `pnpm test -- src/main/services/stopwatch/opusDecoder.test.ts`.
- [ ] Run `pnpm typecheck`.

## Task 5: BLE Service

**Files:**
- Create: `src/main/services/stopwatch/stopwatchBleService.ts`
- Test: `src/main/services/stopwatch/stopwatchBleService.test.ts`

**Interfaces:**
- Consumes: protocol codec and shared UUIDs.
- Produces:
  - `class StopwatchBleService`
  - `connect(deviceId?: string): Promise<StopwatchDeviceState>`
  - `disconnect(): Promise<void>`
  - `getState(): StopwatchDeviceState`
  - `onControl(handler): () => void`
  - `onAudioFrame(handler): () => void`
  - `writeStatus(message: StopwatchStatusMessage): Promise<void>`

- [ ] Write tests using a fake BLE transport so no real Bluetooth hardware is needed.
- [ ] Cover successful connect, capability read, Control notify, Audio notify, Status write, disconnect, and write failure.
- [ ] Implement transport injection so production BLE and tests use the same service logic.
- [ ] Implement state transitions: `disconnected`, `connecting`, `connected`, `error`.
- [ ] Record battery level, firmware version, audio parameters, and last error in state.
- [ ] Run `pnpm test -- src/main/services/stopwatch/stopwatchBleService.test.ts`.
- [ ] Run `pnpm typecheck`.

## Task 6: StopWatch Audio Source

**Files:**
- Create: `src/main/services/stopwatch/stopwatchAudioSource.ts`
- Test: `src/main/services/stopwatch/stopwatchAudioSource.test.ts`

**Interfaces:**
- Consumes: `StopwatchBleService`, `StopwatchJitterBuffer`, `StopwatchOpusDecoder`.
- Produces:
  - `class StopWatchBleAudioSource`
  - `start({ onChunk, onLevel, onError }): Promise<void>`
  - `stop(): Promise<void>`
  - `isRunning(): boolean`

- [ ] Write tests proving that a valid audio frame produces a PCM chunk.
- [ ] Write tests proving that duplicate frames do not produce duplicate PCM chunks.
- [ ] Write tests proving that stop unsubscribes from BLE audio events.
- [ ] Write tests proving decode errors call `onError` and increment diagnostics.
- [ ] Implement audio level calculation from PCM RMS for existing overlay level display.
- [ ] Run `pnpm test -- src/main/services/stopwatch/stopwatchAudioSource.test.ts`.
- [ ] Run `pnpm typecheck`.

## Task 7: VoiceSessionCoordinator External Trigger

**Files:**
- Modify: `src/main/presenter/voiceSessionCoordinator.ts`
- Test: existing or new `voiceSessionCoordinator` test file matching current test layout.

**Interfaces:**
- Consumes: StopWatch semantic button events.
- Produces:
  - `handleExternalVoiceTrigger(payload: ExternalVoiceTriggerPayload): void`

- [ ] Write tests for StopWatch toggle press starting and stopping through the existing toggle path.
- [ ] Write tests for StopWatch PTT down/up using existing PTT path.
- [ ] Write tests for cancel clearing the current recording session without processing partial text.
- [ ] Implement `handleExternalVoiceTrigger` by translating to internal mode handling, not by emitting fake keyboard events.
- [ ] Send Status updates back through a callback hook when state changes to idle, recording, transcribing, final text, or error.
- [ ] Run the relevant `voiceSessionCoordinator` tests.
- [ ] Run `pnpm typecheck`.

## Task 8: Presenter Registration And Default Microphone Selection

**Files:**
- Create: `src/main/presenter/stopwatchDevicePresenter.ts`
- Modify: `src/main/presenter/index.ts`
- Modify: `src/shared/presenter.d.ts`
- Test: presenter tests matching current project conventions.

**Interfaces:**
- Consumes: `StopwatchBleService`, `ConfigPresenter`, `VoiceSessionCoordinator`.
- Produces IPC presenter methods:
  - `getStopwatchStatus(): StopwatchDeviceState`
  - `connectStopwatch(deviceId?: string): Promise<StopwatchDeviceState>`
  - `disconnectStopwatch(): Promise<StopwatchDeviceState>`
  - `setStopwatchAsDefaultMicrophone(): Promise<boolean>`

- [ ] Write tests for setting StopWatch as default microphone and label.
- [ ] Write tests ensuring incompatible Config capability refuses default microphone selection.
- [ ] Implement presenter methods and register `stopwatchDevicePresenter` in `Presenter.registerIpcPresenters`.
- [ ] Wire Control button events into `voiceCoordinator.handleExternalVoiceTrigger`.
- [ ] Wire final Vokie state/text events back to `stopwatchBleService.writeStatus`.
- [ ] Run presenter tests.
- [ ] Run `pnpm typecheck`.

## Task 9: Capture Pipeline Routing

**Files:**
- Modify: `src/renderer/common/pcm/usePcmCapture.ts`
- Modify or create the main/preload bridge required for StopWatch PCM source access after tracing the existing capture path.
- Test: capture routing tests matching the current test layout.

**Interfaces:**
- Consumes: `isStopwatchMicrophoneId(defaultMicrophoneId)`.
- Produces: a capture path where StopWatch PCM chunks are treated like normal microphone PCM chunks by later ASR code.

- [ ] Trace current PCM capture path from `defaultMicrophoneId` through `usePcmCapture.ts` to ASR input.
- [ ] Write a failing test or harness proving `vokie-stopwatch:<deviceId>` does not call browser `getUserMedia`.
- [ ] Implement routing so StopWatch microphone ids use `StopWatchBleAudioSource`.
- [ ] Keep normal microphones and system-audio behavior unchanged.
- [ ] Verify Vokie ASR receives decoded PCM chunks with the same shape as existing microphone chunks.
- [ ] Run capture routing tests.
- [ ] Run `pnpm typecheck`.

## Task 10: Settings UI And Battery Display

**Files:**
- Modify the settings view/component that currently renders microphone selection.
- Modify locale files in `src/shared/i18n/locales/*.ts`.
- Test: component or store tests matching project conventions.

**Interfaces:**
- Consumes: `stopwatchDevicePresenter` IPC methods and StopWatch state.
- Produces:
  - StopWatch settings card.
  - Battery display.
  - Connected/default microphone indicators.
  - Connect/disconnect/default microphone actions.

- [ ] Locate the current settings microphone component.
- [ ] Add StopWatch card copy in Chinese, English, and Japanese locale files.
- [ ] Render status, device name, battery, firmware, audio format, and default microphone state.
- [ ] Add connect, disconnect, and set-default actions.
- [ ] Add StopWatch item in microphone selection when connected.
- [ ] Ensure text fits in compact settings layouts.
- [ ] Run UI tests if available.
- [ ] Run `pnpm typecheck`.

## Task 11: End-To-End Diagnostics And Manual Test Script

**Files:**
- Create: `docs/superpowers/specs/2026-07-09-m5-stopwatch-ble-opus-integration-design.md` update if implementation decisions change.
- Create or modify: a local diagnostics doc under `docs/` if the project has an existing diagnostics pattern.

**Interfaces:**
- Consumes: all prior tasks.
- Produces: manual verification checklist for real StopWatch firmware and Vokie.

- [ ] Add diagnostics logging for connect, disconnect, control event, audio frame gap, decode error, and status write failure.
- [ ] Document real-device manual test steps:
  - Pair StopWatch.
  - Confirm battery appears.
  - Set StopWatch as default microphone.
  - Single-click toggle recording.
  - Long-press PTT recording.
  - Double-click cancel.
  - Confirm final ASR text appears on StopWatch.
  - Disconnect during recording and confirm graceful failure.
- [ ] Run `pnpm typecheck`.
- [ ] Run the StopWatch-related unit tests.
- [ ] Run a real-device smoke test with StopWatch firmware.

## Self-Review Checklist

- Spec coverage:
  - BLE-only Opus audio path is covered by Tasks 2 through 6.
  - Default microphone selection is covered by Tasks 1, 8, 9, and 10.
  - Button mapping is covered by Tasks 2, 7, and 8.
  - Battery display is covered by Tasks 5 and 10.
  - Final ASR text回传 is covered by Tasks 7 and 8.
  - Existing ASR pipeline reuse is covered by Task 9.
- Completion marker scan:
  - No implementation task uses unresolved marker text.
  - The plan intentionally requires locating existing UI/capture files during implementation because those files are part of the current codebase and must be traced before editing.
- Type consistency:
  - Shared helper names and presenter method names are stable across tasks.
  - Audio source output is consistently `Int16Array` PCM.

## Execution Handoff

Plan complete and saved to `docs/superpowers/plans/2026-07-09-m5-stopwatch-ble-opus-integration.md`.

Execution options:

1. Subagent-driven: dispatch a fresh worker per task and review after each task.
2. Inline execution: implement tasks in this session with checkpoints.

Do not start implementation until the spec and plan are reviewed and approved.
