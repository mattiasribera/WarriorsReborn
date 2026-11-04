# Code signing policy

Free code signing provided by [SignPath.io](https://about.signpath.io), certificate by
[SignPath Foundation](https://signpath.org).

WarriorsReborn has applied to the SignPath Foundation open source program. Until a release says
that its Windows programs are signed, the programs are unsigned and Windows SmartScreen or an
antivirus may warn on first run.

## What is signed

Only the Windows programs in the release zip that this project builds from its own source:
`warriors-launcher.exe`, `TheWarriors.exe` and `xbrun-core.dll`. Third-party files shipped
unchanged, such as `SDL2.dll`, are not signed by this project.

Every release is built from a tag of this public repository by GitHub Actions
(`.github/workflows/release.yml`). Nothing is built or signed on a personal computer. No game data
is ever included in a release, so nothing from the game is ever signed.

## Team roles

| Role | Members |
|---|---|
| Authors (committers) | [mattiasribera](https://github.com/mattiasribera) |
| Reviewers | [mattiasribera](https://github.com/mattiasribera): every change from someone who is not an author (for example a pull request) is reviewed before it is merged |
| Approvers | [mattiasribera](https://github.com/mattiasribera): every signing request is approved by hand |

Every team member uses multi-factor authentication for GitHub and SignPath.

## Privacy

This program will not transfer any information to other networked systems unless specifically
requested by the user or the person installing or operating it.

WarriorsReborn has no telemetry, update check or online service. It reads only the game copy,
saves and settings that the player points it at, on the player's own computer.

## Reporting a problem

Report a wrongly signed file, or any other signing problem, by opening an issue at
<https://github.com/mattiasribera/WarriorsReborn/issues>.
