# Building this VR-fixed version on Windows (local build, no cloud)

This is a private copy of [Video simili duplicate cleaner](https://github.com/theophanemayaud/video-simili-duplicate-cleaner)
(GPL-3.0) with one change on the `vr-robustness` branch (also merged to `master`): commit
`fix(video): stop large VR libraries from failing to scan`. See the commit message for details.

Everything below runs on your PC. The first build downloads and compiles Qt, FFmpeg and OpenCV with vcpkg, which
takes **1 to 3 hours** and about **30 GB** of disk. Later builds take a few minutes.

## 1. Install the tools (one time)

1. **Visual Studio 2022 Build Tools**: https://visualstudio.microsoft.com/downloads/ (under "Tools for Visual Studio").
   In the installer, tick **Desktop development with C++**. Keep the default parts (MSVC, Windows SDK,
   *C++ CMake tools for Windows*).
2. **Git for Windows**: https://git-scm.com/download/win (defaults are fine).
3. **vcpkg** (builds the libraries). In a normal Command Prompt:
   ```bat
   git clone https://github.com/microsoft/vcpkg C:\vcpkg
   C:\vcpkg\bootstrap-vcpkg.bat -disableMetrics
   setx VCPKG_ROOT C:\vcpkg
   ```
   Close that window afterwards so the new `VCPKG_ROOT` setting takes effect.

Use short folder paths (like `C:\vcpkg` and `C:\src`). Long paths make the Qt build fail.

## 2. Get the code

```bat
mkdir C:\src
cd C:\src
git clone <this repository's URL> video-simili-vr
```

## 3. Build

Open **"x64 Native Tools Command Prompt for VS 2022"** from the Start menu (not a normal Command Prompt), then:

```bat
cd C:\src\video-simili-vr\QtProject
cmake --preset vcpkg-x64-static
cmake --build build --config Release --parallel --target video-simili-duplicate-cleaner
```

The first command is the long one. When it finishes, the app is:

```
C:\src\video-simili-vr\QtProject\build\Release\video-simili-duplicate-cleaner.exe
```

It is a single standalone .exe. You can copy it anywhere (for example `C:\Tools\VideoSimiliVR\`) and run it directly.
No installer is needed. Windows SmartScreen may warn because it is unsigned: **More info → Run anyway**.

## 4. Optional: run the tests

In the same prompt:

```bat
cmake --build build --config Release --parallel --target test_repo_vr_robustness test_repo_video_matching test_failed_video_cache
ctest --test-dir build -C Release --output-on-failure -R "test_repo_vr_robustness|test_repo_video_matching|test_failed_video_cache"
```

All should pass.

## 5. First scan: empty the cache once

Video simili remembers files that failed in earlier scans and **skips them** ("skipped, cache indicated it had
failed in a previous scan"). So the first time you use this build, choose **Tools → Empty cache**, then scan. Otherwise
the files that used to fail are never retried.

## Updating later

```bat
cd C:\src\video-simili-vr
git pull
cd QtProject
cmake --build build --config Release --parallel --target video-simili-duplicate-cleaner
```

## If the build fails

- `VCPKG_ROOT` not set / toolchain file not found: reopen the x64 Native Tools prompt after step 1.3, or run
  `set VCPKG_ROOT=C:\vcpkg` in the prompt.
- "path too long" or random Qt build errors: make sure the code and vcpkg are in short paths (`C:\src`, `C:\vcpkg`).
- Anything else: copy the last ~30 lines of the output, they show the real error.
