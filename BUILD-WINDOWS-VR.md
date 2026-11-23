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

   If you already have vcpkg in another folder (for example `C:\VCpkg`), use that folder instead of `C:\vcpkg`
   in these commands. Upper/lower case doesn't matter on Windows: `C:\VCpkg` and `C:\vcpkg` are the same folder.

4. **Check that the build can find vcpkg.** Open a **new** Command Prompt and run:
   ```bat
   echo %VCPKG_ROOT%
   dir "%VCPKG_ROOT%\vcpkg.exe"
   ```
   - **It prints your vcpkg folder and finds `vcpkg.exe`:** done, go to step 2.
   - **It prints `%VCPKG_ROOT%` (not set) or a different folder:** set it to your vcpkg folder's full path (open the
     folder in File Explorer and click the address bar to see it), for example:
     ```bat
     setx VCPKG_ROOT "C:\VCpkg"
     ```
     Then close the Command Prompt, open a new one, and run the two check commands again.
   - **`vcpkg.exe` is not found (File Not Found):** vcpkg's one-time setup hasn't run yet:
     ```bat
     "%VCPKG_ROOT%\bootstrap-vcpkg.bat" -disableMetrics
     ```
     Then run the check again.

Use short folder paths (like `C:\vcpkg` or `C:\VCpkg`, and `C:\src`). Long paths, such as folders deep inside
Documents, make the Qt build fail with "path too long" errors. Also keep vcpkg and the code **out of OneDrive-synced
folders**: syncing locks files while they're being built and breaks the build.

## 2. Get the code

```bat
mkdir C:\src
cd C:\src
git clone https://github.com/DROARR/VRvideodedup.git video-simili-vr
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

- **`'cmake' is not recognized as an internal or external command`**: you're in a normal Command Prompt or PowerShell.
  Close it and open **"x64 Native Tools Command Prompt for VS 2022"** from the Start menu (type `x64 Native` to find
  it). Check with `where cmake`: it should print a path inside `Microsoft Visual Studio\2022\BuildTools`.
  - If that prompt isn't in the Start menu, or `where cmake` still finds nothing: open **Visual Studio Installer**,
    click **Modify** on Build Tools 2022, tick **Desktop development with C++**, make sure
    **C++ CMake tools for Windows** is ticked on the right, and click **Modify** to install.
- **`'git' is not recognized`**: install Git for Windows (step 1.2), then open a **new** prompt.

- `VCPKG_ROOT` not set / "Could not find toolchain file" / `vcpkg.cmake` not found: redo the check in step 1.4,
  then open a **new** x64 Native Tools prompt (prompts opened before `setx` don't see the change). For just the
  current prompt you can also run `set VCPKG_ROOT=C:\VCpkg` (your folder, no quotes).
- "path too long" or random Qt build errors: make sure the code and vcpkg are in short paths (`C:\src`, `C:\VCpkg`)
  and not inside OneDrive.
- Anything else: copy the last ~30 lines of the output, they show the real error.
