# Changelog

## [0.2.0](https://github.com/fluxwm/flux/compare/v0.1.0...v0.2.0) (2026-10-27)


### Features

* add ext-data-control-v1 protocol support ([559da63](https://github.com/fluxwm/flux/commit/559da63cf2dcf2dc3312082e6b4f9065d05df4d8))
* add window fullscreen support ([fac2256](https://github.com/fluxwm/flux/commit/fac225685152efc9ca25b58742d9820d7c08c22e))
* add window maximization support ([64e1055](https://github.com/fluxwm/flux/commit/64e1055500120330a7408b1808354349d1ed0b10))
* add wlr-data-control-unstable-v1 protocol support for backward compatibility ([cffbebe](https://github.com/fluxwm/flux/commit/cffbebe4d2330b60c52bd01063ed7d301aea5221))
* add wlr-gamma-control-unstable-v1 protocol support ([3590354](https://github.com/fluxwm/flux/commit/3590354fa3965fb2274c1c09b82833dc4176a777))


### Refactoring

* deduplicate window dimension logic ([05f780b](https://github.com/fluxwm/flux/commit/05f780baf183929dd9021c45890b52a48694f14f))
* extract input subsystem from compositor ([eedbd69](https://github.com/fluxwm/flux/commit/eedbd692818f8a0c5d04403995dca74de344e1da))
* remove redundant in-memory protocol and scene null checks ([30903a7](https://github.com/fluxwm/flux/commit/30903a7bf4a1f2a6d042bb35fbc4177210007b98))


### Continuous Integration

* publish releases immediately instead of drafting ([de2dee1](https://github.com/fluxwm/flux/commit/de2dee1f972a93be05c75f2c7938b3e20e1f04c3))

## [0.1.0](https://github.com/fluxwm/flux/compare/v0.1.0...v0.1.0) (2026-10-26)


### Features

* add autostart configuration support ([5319667](https://github.com/fluxwm/flux/commit/53196676c3a08713e2a3f8b22961847f7f9bd69e))
* add custom logging framework and normalize log messages ([d3225ac](https://github.com/fluxwm/flux/commit/d3225acd6df5f959f0caafefc5e6ffd8434a7b75))
* add debug logging CLI flag and default to info level ([ab2a002](https://github.com/fluxwm/flux/commit/ab2a002e3f4d4774fe74cb5f12199dfb47ee750a))
* add default configuration file ([72047bb](https://github.com/fluxwm/flux/commit/72047bb4a59145367e6d1632b5ffcae90181a69a))
* add hardcoded VT switching ([8e9f163](https://github.com/fluxwm/flux/commit/8e9f163fe705320dad5886e29130a7f255b91819))
* add initial Wayland compositor prototype ([99166e7](https://github.com/fluxwm/flux/commit/99166e7e4d01cb011983ea5035673f594fae876d))
* add interactive mouse drag bindings for window move and resize ([5e2279d](https://github.com/fluxwm/flux/commit/5e2279d35df1800657a738f898c6ea117193195d))
* add wlr-layer-shell-unstable-v1 protocol support ([75d4d77](https://github.com/fluxwm/flux/commit/75d4d77fe337c39c8aa6ead1e3d3a048d75b7c51))
* add xdg-output-unstable-v1 protocol support ([a436569](https://github.com/fluxwm/flux/commit/a436569b78bc64d574ddba2f22c9824ac3b9abab))
* automatically center windows on active output ([e8ee230](https://github.com/fluxwm/flux/commit/e8ee2300c41f151098217fd21ba376c437d76ed3))
* define project version and add version CLI flag ([d4e120d](https://github.com/fluxwm/flux/commit/d4e120d69ac02cd39085b1135383c0c7f4b418ce))


### Bug Fixes

* correct move/resize grab lifecycle and restore cursor on release ([c624271](https://github.com/fluxwm/flux/commit/c62427100d9ada95ee0808e196dd5bc671d6e2ac))
* handle exit signals and terminate on nested output close ([1ab50ea](https://github.com/fluxwm/flux/commit/1ab50ea6004e6245b9f8fd9ad453be2a5f446197))
* prevent file descriptor leaks and restore SIGCHLD in spawn ([b126292](https://github.com/fluxwm/flux/commit/b1262929d5b121578a062d2723d47dba5717549f))
* redirect stdout and stderr to dev null in spawn command ([bb17dc6](https://github.com/fluxwm/flux/commit/bb17dc67c99ca8a9089b8fa56365a8cd09ba6ced))


### Refactoring

* deduplicate window title and app_id extraction ([2bfeb20](https://github.com/fluxwm/flux/commit/2bfeb20da0ce4958bd9202fa34cf854097087699))
* rename Server to Compositor ([4e4c2f8](https://github.com/fluxwm/flux/commit/4e4c2f897862764a5a1efbd7bf10d0e502eae576))
* simplify key combo parsing and keycode tracking with ranges ([ee5d19a](https://github.com/fluxwm/flux/commit/ee5d19a3be9302b10256742cb6df06a39f57ad70))


### Build System

* pin ckdl dependency to commit c9c33fe ([a9f1790](https://github.com/fluxwm/flux/commit/a9f1790d0d38688542da730d593bc3a28ab3bc8f))


### Continuous Integration

* add build and formatting check workflow ([3b6f95b](https://github.com/fluxwm/flux/commit/3b6f95bd49a969a7edefcf180e1735ce042aa21a))
* add clang-tidy check and aggregator job to workflow ([b82900b](https://github.com/fluxwm/flux/commit/b82900b3dd05d96642b5ad47289a99531938f645))
* configure release-please workflow ([0171068](https://github.com/fluxwm/flux/commit/0171068bc3b52bb7a540e6df42677c476bf90661))
