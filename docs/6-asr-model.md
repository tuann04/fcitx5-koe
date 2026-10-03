# ASR Backend

<details>
<summary>Relevant source files</summary>

- [nix/module.nix](../nix/module.nix)
- [daemon/transcribe.cpp](../daemon/transcribe.cpp)
- [nix/config.example.toml](../nix/config.example.toml)

</details>

## Purpose and Scope

Any server that speaks `POST /v1/audio/transcriptions` works as a backend. Local and cloud options can be swapped without code changes by switching the daemon profile.

## Options

| Backend | Notes |
|---|---|
| Local llama-server | Example default. GPU recommended for live partials. Files and ports are set in `nix/module.nix` |
| OpenAI-compatible cloud API | Set `base_url`, `model`, and `api_key_file`. Disable partials to avoid extra billed calls |
| Self-hosted server | Same HTTP API, any model |

## Runtime

- The local example uses llama.cpp with a main GGUF plus an `mmproj` GGUF for the audio encoder.
- Files live under `~/.local/share/koe/models`. See `nix/module.nix` for paths.
- Some backends prefix output with `language <Name><asr_text>`. The daemon strips it.

Sources: [daemon/transcribe.cpp:96-103](../daemon/transcribe.cpp#L96-L103), [nix/module.nix:79-96](../nix/module.nix#L79-L96)

## Measured performance

Example timing with a local GPU backend:

| Setup | 3-4 s clip | 9 s clip |
|---|---|---|
| CPU (llama.cpp, no CUDA) | 8-10 s | not measured |
| RTX 4050 Laptop, CUDA, Q8_0 | 0.2-0.38 s | 0.35 s |

## Known limits

- **Hallucination on non-speech.** On silence or background noise the model can produce fluent text. The daemon's `min_rms` gate blocks true silence. Noise above the gate can still leak into partials.
- **Benchmarks are not your voice.** To choose between local and cloud, record about 20 of your own clips and send them to both with `curl -F file=@clip.wav -F model=... <base_url>/v1/audio/transcriptions`.

## Related pages

- [Transcription](4.2-transcription.md)
- [Deployment](5-deployment.md)
