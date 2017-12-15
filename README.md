node.gl
=======

`node.gl` is a [GoPro][gopro] OpenGL engine for building and rendering
graph-based scenes. It is designed to run on both desktop (Linux, OSX, Windows)
and mobile (Android, iOS).

![node.gl logo](/doc/nodegl.png)

**Warning:** note that `node.gl` is still highly experimental. This means the ABI
and API can change at any time.

[gopro]: https://gopro.com/


## 📜 License

`node.gl` is licensed under the Apache License, Version 2.0. Read the
[LICENSE][license] and [NOTICE][notice] files for details.

**Warning**: `pynodegl-utils` has an optional dependency on PyQt5 which is
licensed under the GPL and thus restrict the `pynodegl-utils` module
distribution.

[license]: /LICENSE
[notice]: /NOTICE

## 📚 Documentation

### 📁 Project

- [Changelog][proj-changelog] includes all the recent changes (features, API
  changes). ⚠️ TODO
- [Developer guidelines][proj-developers] if you are interested in
  contributing.
- [Project architecture and organization][proj-archi]

### 🛠 Tutorial

The [starter tutorial][tuto-start] is a must read before anything else if you
are new to the project.

### 💡 How-to guides

Following are how-to guides on various specific usages:

- [Installation][howto-install]
- [Animations][howto-anim] ⚠️ TODO
- [Make the best use out of mobile video acceleration][howto-mobile-video] ⚠️ TODO
- [Using the C API][howto-c-api]
- [Writing a new node][howto-write-new-node] (for core developers only)

### ⚙️ Discussions and explanations

- [How the Python binding is created][expl-pynodegl]
- [The tree graph structure and its associated rendering context][expl-treegraph] ⚠️ TODO
- [What happens in a draw call?][expl-draw-call]
- [Mobile hardware accelerated video support][expl-mobile] ⚠️ TODO
- [Technical choices][expl-techchoices]
- [Fragment and vertex shader parameters][expl-shaders]

### 🗜 Reference documentation

- [libnodegl][ref-libnodegl]
- [pynodegl][ref-pynodegl]
- [pynodegl-utils][ref-pynodegl-utils]
- [ngl-tools][ref-ngl-tools]

[proj-changelog]:        /doc/project/changelog.md
[proj-archi]:            /doc/project/architecture.md
[proj-developers]:       /doc/project/developers.md
[tuto-start]:            /doc/tuto/start.md
[howto-install]:         /doc/howto/installation.md
[howto-anim]:            /doc/howto/animations.md
[howto-mobile-video]:    /doc/howto/mobile-video.md
[howto-c-api]:           /doc/howto/c-api.md
[howto-write-new-node]:  /doc/howto/write-new-node.md
[expl-pynodegl]:         /doc/expl/pynodegl.md
[expl-treegraph]:        /doc/expl/treegraph.md
[expl-draw-call]:        /doc/expl/draw-call.md
[expl-mobile]:           /doc/expl/mobile.md
[expl-techchoices]:      /doc/expl/techchoices.md
[expl-shaders]:          /doc/expl/shaders.md
[ref-libnodegl]:         /libnodegl/doc/libnodegl.md
[ref-pynodegl]:          /doc/ref/pynodegl.md
[ref-pynodegl-utils]:    /doc/ref/pynodegl-utils.md
[ref-ngl-tools]:         /doc/ref/ngl-tools.md
