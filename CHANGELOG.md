# Changelog

## [0.6.0](https://github.com/fluxwm/flux/compare/v0.5.0...v0.6.0) (2026-10-31)


### Features

* add directional window focus action ([77235e3](https://github.com/fluxwm/flux/commit/77235e365a2bb812510c1c958e6fda728a5ac960))
* add xdg-decoration-unstable-v1 protocol support ([5de1925](https://github.com/fluxwm/flux/commit/5de19251b67ef71b9a9e7189391489b219baeec3))
* embed default config and auto-generate on first launch ([1074d39](https://github.com/fluxwm/flux/commit/1074d394498a9fbc255a28e4cbb987b364c66c72))
* implement master-stack window tiling ([28e28e4](https://github.com/fluxwm/flux/commit/28e28e40701474701783758b5a17cf27e573d7a2))


### Bug Fixes

* clear pointer focus on interactive grabs and restrict cursor under exclusive layers ([7bfc76d](https://github.com/fluxwm/flux/commit/7bfc76de9cddc45b2dd02fd65097c6ed4a43f0c9))
* correct initial tiling geometry and window focus transitions ([1392ee5](https://github.com/fluxwm/flux/commit/1392ee50b01c6632cc0fecec94911a50d01af86b))
* elevate window popups above top panels and safely dismiss on transitions ([be75644](https://github.com/fluxwm/flux/commit/be75644b77e0401f2fa59764ef1939ad64d28d9f))
* improve cursor reload, numlock toggle and layer defocus ([4241bb1](https://github.com/fluxwm/flux/commit/4241bb1bc9428d6aa5b911015f8d8867af79a5c9))
* prevent use-after-free on output removal and correct layer popup lifecycle ([4ef07e2](https://github.com/fluxwm/flux/commit/4ef07e2c330bc4c130304f3cd6d585c7a33e95ba))
* restore topmost fullscreen window per output and correct window lifecycle ([a6f27c2](https://github.com/fluxwm/flux/commit/a6f27c2ed848f9c6a46da8e03cd1c20031cd28ee))

## [0.5.0](https://github.com/fluxwm/flux/compare/v0.4.0...v0.5.0) (2026-10-30)


### Features

* add cursor theme and size configuration ([6c098a2](https://github.com/fluxwm/flux/commit/6c098a21a9702320fd48d449f6af0d58289fa7da))
* add cursor-shape-v1 protocol support ([c9d7592](https://github.com/fluxwm/flux/commit/c9d75929054af4792798c6978134327b08156a00))
* add ext-foreign-toplevel-list-v1 protocol support ([39557c2](https://github.com/fluxwm/flux/commit/39557c2de967303e0bcb14ddffa867afc2d085b3))
* add ext-workspace-v1 protocol support ([4db36c0](https://github.com/fluxwm/flux/commit/4db36c059d64b3f041c6a31efa1e686be4cc928d))
* add fractional-scale-v1 protocol support ([d9050c6](https://github.com/fluxwm/flux/commit/d9050c608e95b4a5adcad09b174008cf97635a16))
* add keyboard numlock configuration ([74ccaf6](https://github.com/fluxwm/flux/commit/74ccaf684db2bbb84386e88298a443033638fc6b))
* add keyboard repeat rate and delay configuration ([87c29ad](https://github.com/fluxwm/flux/commit/87c29ad1eee1bf8bd4545908ad402827c5ca9ca6))
* add workspace support ([8140998](https://github.com/fluxwm/flux/commit/81409989eaa2f87348c2f3935f67729e9d1d5241))
* add xdg-activation-v1 protocol support ([707cc0f](https://github.com/fluxwm/flux/commit/707cc0f5a9d4fff90dc71f43edce7451fa908370))


### Refactoring

* add focusability and surface query helpers to Layer and Window ([e3c3f53](https://github.com/fluxwm/flux/commit/e3c3f53dece515c2d9126faf8529476d18aa0c98))
* deduplicate popup root scene tree resolution ([1e840d5](https://github.com/fluxwm/flux/commit/1e840d528a070b8784e0e9919912b66c6e0ed180))
* deduplicate window geometry configuration and centering ([2d39186](https://github.com/fluxwm/flux/commit/2d39186b7c163f40d7e908653d3ae050537874fd))
* remove unused protocol pointers and redundant null checks ([d1d4ddd](https://github.com/fluxwm/flux/commit/d1d4ddd2583898fc61c12876cf9050f5c61b1f49))
* simplify WlListener and remove virtual IListener interface ([a5d54d4](https://github.com/fluxwm/flux/commit/a5d54d44a90b8c5b8a474a8e691825374ac87ba6))

## [0.4.0](https://github.com/fluxwm/flux/compare/v0.3.0...v0.4.0) (2026-10-29)


### Features

* add linux-dmabuf-v1 protocol support with scene feedback ([62e8b0b](https://github.com/fluxwm/flux/commit/62e8b0b7879705252c339448c65bde9a514a12b3))
* add presentation-time protocol support ([1e1aa4c](https://github.com/fluxwm/flux/commit/1e1aa4c16656de9a9b142506f18ede61a371ff44))
* add single-pixel-buffer-v1 protocol support ([e7087ce](https://github.com/fluxwm/flux/commit/e7087ce15aa8628ce502d14769121118c3d2dc56))
* add viewporter protocol support ([0a63eb6](https://github.com/fluxwm/flux/commit/0a63eb6f5f9f235763668daba91ec50387b0991d))
* bump wl_compositor to version 6 ([c195740](https://github.com/fluxwm/flux/commit/c1957401ef3d0c72095044ae067074bc069711b7))


### Bug Fixes

* always send frame done in output frame handler ([ac3a68f](https://github.com/fluxwm/flux/commit/ac3a68fdc468f6805ff5bdc6ced38070ec698f22))
* correct CSD interactive resize grab offsets and fallback geometry ([b9d5e22](https://github.com/fluxwm/flux/commit/b9d5e220f3d1d5bb581e079ffef3cbaaa5632fee))
* elevate popups above layer surfaces and map layer popups to global layout ([35aeabd](https://github.com/fluxwm/flux/commit/35aeabd6d7c5071778f01f1becc1f8bb9877fc56))
* guard VT switching keybindings against null session ([d15b621](https://github.com/fluxwm/flux/commit/d15b62182026c2fc946f08ead946cbb6d3e17c01))
* refresh cursor focus on window focus and layer geometry transitions ([d8040e0](https://github.com/fluxwm/flux/commit/d8040e0dc224328568c7f5a5da0533d9efc518d8))
* rotate focus stack on cycle-window action ([3881b29](https://github.com/fluxwm/flux/commit/3881b29a0b9ce4a769a6a3ed392a51bb5d932f58))
* synchronize fullscreen stacking and preserve window positions across output changes ([d66d631](https://github.com/fluxwm/flux/commit/d66d63199549c1b1c3c624a91b67985ef858b32c))


### Refactoring

* add domain query methods and simplify entity wrappers ([e570534](https://github.com/fluxwm/flux/commit/e57053461bb4ddc7b0e1ff8d9dddbb83aa0259bd))
* extract config path resolution and inotify watcher from compositor ([cb9358c](https://github.com/fluxwm/flux/commit/cb9358c57b64298b81664aa2ab8a33e6ffc5574c))
* extract scene graph subsystem from compositor ([5cf07c2](https://github.com/fluxwm/flux/commit/5cf07c25f867a8cf2c9e77492ab067da7e294634))

## [0.3.0](https://github.com/fluxwm/flux/compare/v0.2.0...v0.3.0) (2026-10-28)


### Features

* add ext-image-copy-capture-v1 protocol support ([758151a](https://github.com/fluxwm/flux/commit/758151a2f59300e8d7c4432dcaae4ae631252123))
* add inotify config watcher for automatic reloading ([f45c05e](https://github.com/fluxwm/flux/commit/f45c05e5f58303a32afcf52382d2f988e81e8480))
* add wlr-export-dmabuf-unstable-v1 protocol support ([a1016ee](https://github.com/fluxwm/flux/commit/a1016ee9db0297595a75ef04df1db211b669550d))
* add wlr-screencopy-unstable-v1 protocol support for backward compatibility ([d79937f](https://github.com/fluxwm/flux/commit/d79937f348006b43ef4c9f131248780e4188da7d))
* support executing multiple actions in a keybind block ([b8d4098](https://github.com/fluxwm/flux/commit/b8d40989f7796e338fa57f885919658e1d09c0b7))


### Bug Fixes

* correct maximize and fullscreen configure lifecycle and geometry ([ac4fc72](https://github.com/fluxwm/flux/commit/ac4fc724d9f1d89f7a57cb300fbe48ef2cf2b7eb))
* demote unfocused fullscreen windows on focus change ([78f0b5b](https://github.com/fluxwm/flux/commit/78f0b5b23cc21ae60199204c6edce9c0d2db1ac6))
* ensure keyboard focus on device attach and preserve active keyboard on removal ([3d72f9f](https://github.com/fluxwm/flux/commit/3d72f9f99d0f34c6379df820c9d012c95f47a541))
* ignore maximize requests while fullscreen and synchronize toplevel state ([eb63863](https://github.com/fluxwm/flux/commit/eb63863afd880d20c7b2c6f1199c662a9e7a4012))
* prevent crash and duplicate popup creation for layer-shell surfaces ([3041313](https://github.com/fluxwm/flux/commit/30413135124ec46f3469e06cb9dc3c42446e9de8))
* reset signal mask and handle fork failure in spawn ([55b36f1](https://github.com/fluxwm/flux/commit/55b36f11dedbe5ffa5cf7e23bff60029f4afb328))
* resolve owning window for subsurfaces in window_at ([f450999](https://github.com/fluxwm/flux/commit/f4509999adf318b6e232deb5c3f508527b391a61))

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
