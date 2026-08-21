# AGENTS.md

A desk pet. A face on a 360x360 round LCD on an ESP32-S3, living on the panel on
its own.

The host side below is still the 8x8 RGB matrix this started as - scenes, wire
protocol, daemon, hooks. It is kept for what is in it and nothing on the board
reads any of it.

| | |
| --- | --- |
| `firmware/` | all of the pet that is on the board - panel, face, behaviour |
| `packages/matrix` | the panel: wire protocol, serial port, framebuffer, colour |
| `packages/pet` | what the pet *is*: state, mood, faces, scenes, sessions |
| `packages/tools` | executables that answer one question or set one thing |
| `apps/daemon` | the one process that holds the port and renders |
| `.claude/hooks` | where the status and the quotas come from, on their own |

## Stack

Turborepo with npm workspaces. TypeScript, Node 24, `serialport`. Biome for lint
and formatting, knip for dead code, vitest for specs. The firmware is Arduino via
PlatformIO. The toolchain lives in the root manifest; anything imported at
runtime lives beside what imports it.

## Commands

Run from the repo root - turbo fans them out.

| | |
| --- | --- |
| `npm run pet` | start the daemon, detached |
| `npm run pet:stop` | stop it, clearing the panel |
| `npm run pet:restart` | after changing anything it renders |
| `npm run lint:fix` | lint and format |
| `npm run typecheck` | typecheck |
| `npm test` | vitest across every workspace |
| `npx knip` | dead code and unused dependencies |
| `npm run firmware:build` | compile the sketch |
| `npm run firmware:flash` | and put it on the board |

```bash
npx status-show          # everything at once - start here when the panel is dark
npx status-preview       # the panel, in a terminal, with no board
npx status-install       # wire the hook into every project, not just this one
npx status-play dance
npx status-pixel 0,0,#ff0000
```

## Rules

- **NEVER** bypass pre-commit hooks (`--no-verify`, `LEFTHOOK=0`)
- **NEVER** call biome directly - use `npm run lint` or `npm run lint:fix`
- **NEVER** commit without being explicitly asked
- **NEVER** put a Claude or co-author trailer on a commit
- Atomic commits, conventional messages, max 100 chars per line
- Prefer the smallest change that does the job
- Find root causes - no temporary or hacky fixes

## Guides

- [Hardware](.agents/docs/hardware.md) - the board, its wiring, flashing, and the
  things that cost a day
- [Wire Protocol](.agents/docs/protocol.md) - packets, frame order, and why gamma
  is applied where it is
- [State](.agents/docs/state.md) - sessions, precedence, expiry, and where
  validation lives
- [Code Standards](.agents/docs/code-standards.md) - style, TypeScript, comments

## Skills

- [`claude-pet`](.agents/skills/claude-pet/SKILL.md) - bring the panel up, read
  it, and keep it running for a session. Symlinked into `.claude/skills`.

## Constraints

Invisible in the code until they are broken.

- **One process owns the serial port.** Two daemons both write frames and the
  panel tears between them, which reads as a bad cable rather than as a second
  process - hence the pid lock. Everything else writes a file and leaves, which
  is what lets a hook fire and return whether or not the pet is running
- **Nothing in a session may wait on, or fail because of, the display.** The
  hooks are plain node with no imports because tsx costs 290ms against node's
  34ms and they run on every tool call. They never print and always exit 0
- **The daemon validates; the writers do not.** `state.json` is hand editable and
  hook written, so every field is checked on read. That is what makes it safe for
  the hook to stay dumb
- **Rows 0 and 7 are numbers, and an accent may only ever brighten them.** The
  week is on top, the rolling 5h window on the bottom, and the spinner crosses
  both on purpose. A spec holds the invariant that nothing shortens a bar - one
  that something could eat into would read as a smaller number, and those two
  rows are the only things here that have to be true
- **The bars are account-wide; the face is not.** The face runs on the context
  window of the session being shown, so it still behaves like the chat you are
  in. The two quotas are the same figures in every chat, which is what lets a row
  show one without saying whose it is. A quota nothing has reported stays dark
  and a bar is never shorter than one pixel, so a dark row means "no reading"
  and nothing else - "none of the week used" is the one wrong answer that looks
  like good news
- **`rate_limits` reaches a statusline and nothing else.** No hook payload
  carries it, it never lands in a transcript, and nothing caches it on disk, so
  holding the statusline slot is the only way to see it. There is one slot, so
  `status-install` wraps whatever was in it rather than replacing it
- **The face is the six rows between the bars, and its eyes are top-aligned.**
  Centred on the face rows they still read as low, because an eye takes the
  panel's centre for the face's centre
- **Either quota at 100% takes the panel over**, alternating the cross and a dead
  face in the error red, with antics suppressed - and the cross is out of the
  random pool because of it. A full context window is the pet running out of
  head and the face says it; a spent quota is the account having nothing left,
  which no face can say
- **Scenes are pure functions of time and state.** No scene carries anything
  between frames - the twinkle is hashed from the pixel and the step rather than
  from `Math.random` - which is why a spec can assert what was drawn
- **Gamma is applied at the wire, not in scenes**, so scene arithmetic stays in
  the space a human judges brightness by
- **The face lives on the board.** A 360x360 frame at 16 bits is 253KB, and no
  serial link streams that thirty times a second - so the scene, the timing and
  the mood are all on the S3 and iterating on behaviour costs a reflash rather
  than a restart. That is the reverse of what the matrix did, and it is the
  resolution that forced it rather than a change of mind
- **A shape is a distance, not a span of pixels.** Every part of the face is a
  signed distance field, so coverage falls out of the distance and the edges are
  smooth without a second pass - and an eye becomes a squint, or a smile becomes
  a surprised o, by moving a number rather than by branching to another shape.
  It is also what makes the renderer affordable: one probe at the centre of a
  tile bounds the whole tile, which clears most of the panel without touching it
- **Which way up the panel sits cannot be verified from here**, and every face is
  wrong in three of the four cases. `npx status-orient` draws a marker that
  cannot be read two ways; someone with eyes on it decides
