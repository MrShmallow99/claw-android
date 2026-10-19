# Build environment for the Android APK (used by tools/build.ps1, tools/build-docker.sh
# and the CI workflow). It contains only the toolchain; the
# sources and your game files are mounted at run time.
#
#   docker build -t claw-android-build .
#   docker run --rm -v "$PWD:/src" -v "/path/to/pack.zip:/game/pack.zip:ro" \
#       claw-android-build tools/build.sh picker --game /game/pack.zip
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build pkg-config \
        python3 python3-venv python3-pip \
        git curl ca-certificates unzip p7zip-full xz-utils \
        openjdk-17-jdk-headless \
    && rm -rf /var/lib/apt/lists/*

# Android SDK command-line tools, platform, build tools and NDK r27c
ENV ANDROID_HOME=/opt/android-sdk
ENV ANDROID_NDK_HOME=/opt/android-sdk/ndk/27.2.12479018
ENV PATH=$PATH:/opt/android-sdk/cmdline-tools/latest/bin
RUN curl -fsSL -o /tmp/clt.zip https://dl.google.com/android/repository/commandlinetools-linux-11076708_latest.zip \
    && echo "2d2d50857e4eb553af5a6dc3ad507a17adf43d115264b1afc116f95c92e5e258  /tmp/clt.zip" | sha256sum -c - \
    && mkdir -p $ANDROID_HOME/cmdline-tools \
    && unzip -q /tmp/clt.zip -d $ANDROID_HOME/cmdline-tools \
    && mv $ANDROID_HOME/cmdline-tools/cmdline-tools $ANDROID_HOME/cmdline-tools/latest \
    && rm /tmp/clt.zip \
    && yes | sdkmanager --licenses > /dev/null \
    && sdkmanager --install "platforms;android-36" "build-tools;34.0.0" "build-tools;35.0.0" "ndk;27.2.12479018" > /dev/null

# the mounted source tree belongs to the host user
RUN git config --system --add safe.directory '*'

WORKDIR /src
CMD ["tools/build.sh", "--help"]
