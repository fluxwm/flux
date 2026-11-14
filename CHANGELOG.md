# Changelog

## [0.9.1](https://github.com/fluxwm/flux/compare/v0.9.0...v0.9.1) (2026-11-14)


### Bug Fixes

* allow directional focus from floating windows to background windows ([54192dd](https://github.com/fluxwm/flux/commit/54192dde6086ce6f7090133680b3d377c7be9d69))
* allow focusing tiled and floating windows over fullscreen windows ([6f0288b](https://github.com/fluxwm/flux/commit/6f0288bd7030d034d0a431102e14f5473e81fecb))
* clamp interactive window resize dimensions to client size hints ([3a68ccc](https://github.com/fluxwm/flux/commit/3a68ccca45b83e491f0ee4b1b15e9bf8d5de425f))
* ignore disabled outputs and restore focus on output power changes ([16483e7](https://github.com/fluxwm/flux/commit/16483e7622e5ae33504b2fc01e29fc44d1e4df49))
* place XWayland override-redirect popups in window popups layer ([46aa06f](https://github.com/fluxwm/flux/commit/46aa06f5f46eaab681f044860d881cb39fe49bda))
* preserve floating placement on center and clear stale resize edges ([8f15f65](https://github.com/fluxwm/flux/commit/8f15f655c3ea8c4ab04ff4affe5de1c15130cb72))
* refresh cursor focus when mapped window geometry changes on commit ([e652452](https://github.com/fluxwm/flux/commit/e6524529e9b99dc392ab3262f6c88bcdc4ea7db6))
* resolve constrained surface buffer coordinates in warp_to_hint ([c0d50b8](https://github.com/fluxwm/flux/commit/c0d50b8340a0a25249c8c66e29d7e8348ddb991a))
* resolve keybindings across all configured keyboard layouts ([b466636](https://github.com/fluxwm/flux/commit/b466636c90e2f06bd8620eafe76f959d6c35e3be))
* suppress pointer gesture events when workspace swipe is cancelled ([c888219](https://github.com/fluxwm/flux/commit/c88821980108e01ec872343bf6722dcc5a5dd1d4))
* track spawned child PIDs to prevent XWayland SIGCHLD blocking ([3858546](https://github.com/fluxwm/flux/commit/38585462ebd8552e29f2afa412ed1e7b73ec9c98))
* validate enabled output before clearing focus on workspace swipe ([8042cbb](https://github.com/fluxwm/flux/commit/8042cbb3667e53b4fe351eb0973a6fbd2ad0a8b9))

## [0.9.0](https://github.com/fluxwm/flux/compare/v0.8.0...v0.9.0) (2026-11-14)


### Features

* add native XWayland support ([8a34e06](https://github.com/fluxwm/flux/commit/8a34e06de4d84c59ec0c071b686d88dbf1304455))
* add repeat and locked keybind options ([5e92727](https://github.com/fluxwm/flux/commit/5e92727774070f7429920130579501ee034993b2))
* add virtual-keyboard-unstable-v1 protocol support ([6dbb4f8](https://github.com/fluxwm/flux/commit/6dbb4f89c9b1fbad55fbe5f5cf41158c228e6b60))
* add wayland drag-and-drop support ([cf26374](https://github.com/fluxwm/flux/commit/cf26374496cecb4b959b4392fda9e2f5aaa44139))
* add wlr-output-power-management-unstable-v1 protocol support ([7a19be3](https://github.com/fluxwm/flux/commit/7a19be33c1b5113da996ab55768bc488236d3202))
* add wlr-virtual-pointer-unstable-v1 protocol support ([4924886](https://github.com/fluxwm/flux/commit/49248860d7e2202797ffaa671fe2eb96aadfb3a8))


### Bug Fixes

* avoid reaping XWayland server process in SIGCHLD handler ([f2930e2](https://github.com/fluxwm/flux/commit/f2930e20c37267d69e50da2fd95e8055b3319dea))
* clean up scene output and layout on output destruction ([398135c](https://github.com/fluxwm/flux/commit/398135ce2e5b4911000cb0e9a2c63940d9cc039f))
* clean up XWayland subsurface tree on dissociate and remove redundant arrange ([66a2cde](https://github.com/fluxwm/flux/commit/66a2cde16fc90fae67062f05093e85f89fd1f09a))
* clip sliding workspaces and window borders to output bounds ([174256f](https://github.com/fluxwm/flux/commit/174256fd0a10f3fd5203f8484e0b23d96e70fe96))
* configure XWayland geometry with target box and restack all tiled windows ([fa3db36](https://github.com/fluxwm/flux/commit/fa3db36a21629662fd2a09edf70fd8b207ad38a5))
* destroy child popups before parents and clear XDG surface data ([cc96b9e](https://github.com/fluxwm/flux/commit/cc96b9e1aef9afad27c672c58cddf6e1242f293c))
* fallback to surface input region for uncommitted pointer constraints ([a74a8da](https://github.com/fluxwm/flux/commit/a74a8dab8859eff907fac005fca7350061fdbb1f))
* filter consumed keybind keycodes from keyboard enter events ([d3528f6](https://github.com/fluxwm/flux/commit/d3528f6c0c38df7f52ad151c7e8760b211d3d215))
* guard subsurface tree clipping against empty workspace trees ([da39c98](https://github.com/fluxwm/flux/commit/da39c98b1d04dce0d4136ffa2574670e8c88f9a9))
* keep fullscreen windows above floating windows during workspace animations ([7da01d1](https://github.com/fluxwm/flux/commit/7da01d10a575e563aa38007f6e1b8de9fce3cfdc))
* preserve focus stack order across workspaces on output removal ([217cdec](https://github.com/fluxwm/flux/commit/217cdec3363b85e8f9a53374ebb340116a9e6bde))
* preserve focus stack order when reconnecting outputs ([af556dc](https://github.com/fluxwm/flux/commit/af556dce9edf86bcb3886cbb26c8f45af0e260bf))
* preserve pointer focus on layer surfaces during workspace animations ([6431801](https://github.com/fluxwm/flux/commit/643180119c5530fc8a4173da0d8fb5e3bc352528))
* preserve XWayland floating geometry and correct window state transitions ([0665514](https://github.com/fluxwm/flux/commit/06655141a1b75bf956821827e464e29e5f965820))
* refresh cursor focus on popup transitions and constraint destruction ([52cf91e](https://github.com/fluxwm/flux/commit/52cf91e14e059a25ac9d56e4c9980b5dab76ebdc))
* reject unknown modifiers and empty tokens in keybind combos ([7736576](https://github.com/fluxwm/flux/commit/7736576841a01925b4ca78363698a447e439015a))
* remove stale inotify watch descriptors on config watch updates ([9460258](https://github.com/fluxwm/flux/commit/9460258d6f56d204da0590fbdc80147ae5d0ba52))
* remove subsurface workspace clipping and border clamping during animations ([622da11](https://github.com/fluxwm/flux/commit/622da11c6d6658e98c2a8f841e6f1be58077759e))
* reset placement and refocus when parent window moves across outputs ([7aaff19](https://github.com/fluxwm/flux/commit/7aaff1959f586f8aac2bc8e787e0683c2e7c1e82))
* suppress pointer focus during animations and allow multi-output exclusive layers ([1720929](https://github.com/fluxwm/flux/commit/17209296e37099f258ea2709a2620ea0f702cd92))
* synchronize xwayland maximize state and guard override-redirect popups ([9a7b8bd](https://github.com/fluxwm/flux/commit/9a7b8bdc5ba96a65876abd2ffa8a49822de76abe))
* synchronize XWayland stacking order and restack inactive workspaces ([3f1dec7](https://github.com/fluxwm/flux/commit/3f1dec77fd83ab505b7f7ccb477a9480c52490f5))

## [0.8.0](https://github.com/fluxwm/flux/compare/v0.7.0...v0.8.0) (2026-11-11)


### Features

* add 1:1 workspace swipe gesture support ([4960f29](https://github.com/fluxwm/flux/commit/4960f292bdc3067a5990f13a3c450eef07f609e8))
* add automatic floating window support and centering ([ffaa663](https://github.com/fluxwm/flux/commit/ffaa66359c6443f2c11a396ce0c6d1fb49595318))
* add color-representation-v1 protocol support ([813e09f](https://github.com/fluxwm/flux/commit/813e09f6d30c38b5511eadf8507900ebd1e5d3b9))
* add content-type-v1 protocol support ([e2fa48c](https://github.com/fluxwm/flux/commit/e2fa48ca2a19fffadd18cce47c8f83a01b8c985c))
* add dedicated per-monitor workspace support ([9e86395](https://github.com/fluxwm/flux/commit/9e863953892a109cfbaa704f95c2feb7bcbbb34f))
* add interactive drag and resize for floating windows ([bd0267e](https://github.com/fluxwm/flux/commit/bd0267e14d336af5e1ba3600900396f3aa26d63d))
* add linux-drm-syncobj-v1 protocol support ([f263f2c](https://github.com/fluxwm/flux/commit/f263f2ca323124e726f8e860afee0665281d827c))
* add pointer-gestures-unstable-v1 protocol support ([3b68e90](https://github.com/fluxwm/flux/commit/3b68e90ab0e312cafb71848cef31f8231a7a215a))
* add spring-physics workspace slide and swipe animations ([d72a0f4](https://github.com/fluxwm/flux/commit/d72a0f42dcb01d4b64b9131b8130727730f816c9))
* add toggle-floating window  action ([2c87a29](https://github.com/fluxwm/flux/commit/2c87a29ae5caad81b94a0cb715b8d952a5135caf))
* add window border configuration ([a563f2b](https://github.com/fluxwm/flux/commit/a563f2b2c7ca40f0027315233afabf1af97cc1a6))


### Bug Fixes

* allow workspace swipe to complete on gesture cancel ([9605303](https://github.com/fluxwm/flux/commit/96053030965d27504d93af0212eef832a312a97e))
* reset fullscreen and maximized state on window unmap ([428f439](https://github.com/fluxwm/flux/commit/428f439ce879b1c19b812d0e3aba730b85cf9158))
* validate root scene tree before constructing popups ([9a44ffb](https://github.com/fluxwm/flux/commit/9a44ffbd5f13219bc907d6843cd41955b5e85792))


### Performance Improvements

* cache timezone lookup in logger ([2927bf2](https://github.com/fluxwm/flux/commit/2927bf2d2a9d2b347c7c99be1f1991519c250d43))


### Refactoring

* delegate layout arrangement, restacking, and directional focus to Workspace ([59f645c](https://github.com/fluxwm/flux/commit/59f645c9e03b1e4d9e6a12bcb514c24e6ccef1c8))
* move pointer gestures and relative pointer protocols from Seat to Cursor ([d028191](https://github.com/fluxwm/flux/commit/d028191ed6546bea8085427f82ef15fdc07d82b9))
* simplify kdl node parsing with out-reference helpers ([59cb653](https://github.com/fluxwm/flux/commit/59cb653595c907cf69ba6a1fd07a27f86aa6e07c))
* transfer layer ownership and arrangement from Compositor to Output ([7b0e117](https://github.com/fluxwm/flux/commit/7b0e117a7e109bd5617d5ef3ce13daf7c2dc33ee))
* unify window workspace assignment and remove redundant output pointer ([4821c36](https://github.com/fluxwm/flux/commit/4821c36b3a24acec34cca87a41c1068c3a254aa7))


### Continuous Integration

* use git ls-files for clang-format check ([897ebd3](https://github.com/fluxwm/flux/commit/897ebd303f52d4f807005d19d51f2b07e5b99e3e))

## [0.7.0](https://github.com/fluxwm/flux/compare/v0.6.0...v0.7.0) (2026-11-07)


### Features

* add alpha-modifier-v1 protocol support ([2cbae68](https://github.com/fluxwm/flux/commit/2cbae683de9059482303f5eae3cbc8148cf4cfa3))
* add cmake install targets, session desktop entry, and portals config ([46877dd](https://github.com/fluxwm/flux/commit/46877ddc41cb01b6283186662e1d7d9281720a70))
* add environment configuration support ([5255774](https://github.com/fluxwm/flux/commit/5255774d763f228e4a2217a1a33e3ffa2e667466))
* add ext-idle-notify-v1 protocol support ([97be3af](https://github.com/fluxwm/flux/commit/97be3afcd0140ad0d2e047e5cbe931ab2b6c030a))
* add idle-inhibit-unstable-v1 protocol support ([b9159e9](https://github.com/fluxwm/flux/commit/b9159e9603aa9ebfb55f3c930660e7bc9fa7629d))
* add middle-click-paste configuration ([2698a6c](https://github.com/fluxwm/flux/commit/2698a6cd746f593dae68c52de89850282372d827))
* add monocle layout support ([7f2f26f](https://github.com/fluxwm/flux/commit/7f2f26f66b81f9551e2a146d08437f3786b3cf23))
* add pointer-constraints-unstable-v1 protocol support ([e667f9d](https://github.com/fluxwm/flux/commit/e667f9d2c7a30f19005bded1f759e9afe34a2ef7))
* add primary-selection-v1 protocol support ([c662f1d](https://github.com/fluxwm/flux/commit/c662f1d655d5f3faeeba63f9f06bb0d3c05e0292))
* add relative-pointer-unstable-v1 protocol support ([2291358](https://github.com/fluxwm/flux/commit/22913586098e6491447973e9b5e47e467b49e47c))
* add smart gaps option to layout configuration ([3ffa1bb](https://github.com/fluxwm/flux/commit/3ffa1bb56503aae759fe41cd1c400b82d4775b7d))
* add tearing-control-v1 protocol support ([f73d333](https://github.com/fluxwm/flux/commit/f73d333016db764996f39f37e18fa8f7f5e5923c))
* add touchpad natural-scroll configuration ([c306e16](https://github.com/fluxwm/flux/commit/c306e1662e50d62274d07e411a76a89d0871d2a9))
* add touchpad tap-to-click configuration ([dc285f4](https://github.com/fluxwm/flux/commit/dc285f40122f31104dd6563b9704909d7b7e344c))
* add wl-fixes protocol support ([9270c80](https://github.com/fluxwm/flux/commit/9270c805c5490cbbec670ef4559981a3751f24e6))
* add xdg-foreign-unstable-v1 and v2 protocol support ([ceb0f60](https://github.com/fluxwm/flux/commit/ceb0f60153d4b78d6897638d88d57de6fbb09afa))
* add xkb keymap configuration and fallback safety ([28edfb0](https://github.com/fluxwm/flux/commit/28edfb08b19d73c8024bb6ecf874d28806ffb34f))


### Bug Fixes

* avoid O_CLOEXEC when opening /dev/null in spawn ([1f55f00](https://github.com/fluxwm/flux/commit/1f55f00cfde68dc5773c7fbf861336b0bfc9d665))
* export configured environment variables to dbus and systemd ([0632529](https://github.com/fluxwm/flux/commit/063252918748fc8cac447613c51c4cc6eceddc22))
* re-apply active cursor shape on config reload ([5956d7f](https://github.com/fluxwm/flux/commit/5956d7f87c8eef8c3ddd3b53d00c966ed7975373))


### Refactoring

* align scene tree member declaration order with z-order ([0057e20](https://github.com/fluxwm/flux/commit/0057e2033493dcac34d5b45b71c0228e4b325112))
* move default config into resources directory ([8eb6457](https://github.com/fluxwm/flux/commit/8eb64570ef5b66839da51c5a03958d5f671b18e4))

## [0.6.0](https://github.com/fluxwm/flux/compare/v0.5.0...v0.6.0) (2026-11-02)


### Features

* add default media, brightness, and launcher keybindings ([7344aa6](https://github.com/fluxwm/flux/commit/7344aa650f3f83a4a8b7c93a11589ababa87e27c))
* add directional window focus action ([77235e3](https://github.com/fluxwm/flux/commit/77235e365a2bb812510c1c958e6fda728a5ac960))
* add layout gaps configuration ([063cb7f](https://github.com/fluxwm/flux/commit/063cb7f9637c0dd7626258d77ce2db0243eb3d5c))
* add spawn-sh action ([909f7b0](https://github.com/fluxwm/flux/commit/909f7b0d4bd4b83443b7ae0d272d762784a385e1))
* add xdg-decoration-unstable-v1 protocol support ([5de1925](https://github.com/fluxwm/flux/commit/5de19251b67ef71b9a9e7189391489b219baeec3))
* embed default config and auto-generate on first launch ([1074d39](https://github.com/fluxwm/flux/commit/1074d394498a9fbc255a28e4cbb987b364c66c72))
* export session environment variables and update dbus activation ([8aac4d5](https://github.com/fluxwm/flux/commit/8aac4d554f9e1558e3939c5d753f4569cbc84f4b))
* implement master-stack window tiling ([28e28e4](https://github.com/fluxwm/flux/commit/28e28e40701474701783758b5a17cf27e573d7a2))


### Bug Fixes

* clear pointer focus on interactive grabs and restrict cursor under exclusive layers ([7bfc76d](https://github.com/fluxwm/flux/commit/7bfc76de9cddc45b2dd02fd65097c6ed4a43f0c9))
* correct initial tiling geometry and window focus transitions ([1392ee5](https://github.com/fluxwm/flux/commit/1392ee50b01c6632cc0fecec94911a50d01af86b))
* elevate window popups above top panels and safely dismiss on transitions ([be75644](https://github.com/fluxwm/flux/commit/be75644b77e0401f2fa59764ef1939ad64d28d9f))
* improve cursor reload, numlock toggle and layer defocus ([4241bb1](https://github.com/fluxwm/flux/commit/4241bb1bc9428d6aa5b911015f8d8867af79a5c9))
* prevent use-after-free on output removal and correct layer popup lifecycle ([4ef07e2](https://github.com/fluxwm/flux/commit/4ef07e2c330bc4c130304f3cd6d585c7a33e95ba))
* restore topmost fullscreen window per output and correct window lifecycle ([a6f27c2](https://github.com/fluxwm/flux/commit/a6f27c2ed848f9c6a46da8e03cd1c20031cd28ee))


### Documentation

* add project banners ([7006481](https://github.com/fluxwm/flux/commit/70064816a5b3165cee43449fe41304afbc5ebc1f))

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
