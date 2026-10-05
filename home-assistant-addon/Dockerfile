FROM python:3.13-slim

ARG BUILD_VERSION
ARG BUILD_ARCH
# Where the web server source comes from — set in build.yaml.
ARG SOURCE_REPO=ontheview/mammotion-rc
ARG SOURCE_REF=main

LABEL \
    io.hass.version="${BUILD_VERSION}" \
    io.hass.type="app" \
    io.hass.arch="${BUILD_ARCH}"

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        git \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Changes whenever SOURCE_REF gets a new commit, so a Rebuild in Home Assistant
# re-clones instead of reusing a cached (stale) clone layer.
ADD https://api.github.com/repos/${SOURCE_REPO}/commits/${SOURCE_REF} /tmp/source-version.json

RUN git clone --depth 1 --branch "${SOURCE_REF}" \
    "https://github.com/${SOURCE_REPO}.git" \
    /opt/mammotion-rc

RUN python -m pip install --no-cache-dir --upgrade pip \
    && python -m pip install --no-cache-dir \
       -r /opt/mammotion-rc/web-server/requirements.txt

ENV PYTHONUNBUFFERED=1

COPY run.sh /run.sh

RUN chmod a+x /run.sh

CMD ["/run.sh"]
