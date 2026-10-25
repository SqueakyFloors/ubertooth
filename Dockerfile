# Sandbox test environment for Ubertooth
FROM ubuntu:22.04
USER root

# Override interactive installations and install prerequisite programs
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y \
    cmake \
    gcc \
    gcc-arm-none-eabi \
    git \
    g++ \
    libbluetooth-dev \
    libnewlib-arm-none-eabi \
    libusb-1.0-0-dev \
    make \
    pkg-config \
    python3-distutils \
    python3-numpy \
    python3-pip \
    python3-qtpy \
    python3-setuptools \
    python3-yaml \
    wget \
    && rm -rf /var/lib/apt/lists/*
RUN pip3 install git+https://github.com/CapableRobot/CapableRobot_USBHub_Driver --upgrade

# libbtbb comes from this project's fork, pinned to a commit so a push there
# cannot change this image. The pin is bumped in .github/workflows/build.yml,
# which checks that this copy agrees with it. ENABLE_PYTHON=OFF skips
# libbtbb's pcaptools: the Ubertooth tools link only the C library.
ARG LIBBTBB_URL=https://github.com/SqueakyFloors/libbtbb.git
ARG LIBBTBB_REF=84ab41fc99d172e812438bc9188f30a67216f748
RUN git clone "$LIBBTBB_URL" libbtbb &&\
    cd libbtbb &&\
    git checkout "$LIBBTBB_REF" &&\
    mkdir build &&\
    cd build &&\
    cmake -DENABLE_PYTHON=OFF .. &&\
    make &&\
    make install &&\
    ldconfig

# Inform Docker that the container is listening on port 8080 at runtime.
EXPOSE 8080
