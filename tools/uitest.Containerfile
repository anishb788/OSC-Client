# The container the PC build and host tests run in (the host doesn't need the SDL2/jansson/libzip dev packages).
#   podman build -t openshop-uitest -f tools/uitest.Containerfile tools
# Keep the package list in step with the apt line in README.md.
FROM docker.io/library/debian:trixie
RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends g++ make pkg-config python3 \
    libsdl2-dev libsdl2-ttf-dev libsdl2-mixer-dev libsdl2-image-dev libjansson-dev libzip-dev \
    xvfb xdotool imagemagick >/dev/null && rm -rf /var/lib/apt/lists/*
