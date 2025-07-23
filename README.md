# libairplay2
AirPlay2 player and library

## Building (debian OS, cross compile)

```sh
apt-get update
apt-get install -y build-essential cmake  libssl-dev libevent-dev libgcrypt20-dev
git clone https://github.com/bradkeifer/libairplay2.git
cd libairplay2
#git submodule update --init

# Build project
./build.sh
```

## Building (alpine musl build)

```sh
apk add --update alpine-sdk build-base openssl-dev libevent-dev libgcrypt-dev
git clone https://github.com/bradkeifer/libairplay2.git
cd libairplay2

# Build for architecture
make HOST=linux PLATFORM=aarch64
```
