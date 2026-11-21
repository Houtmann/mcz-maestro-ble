# PlatformIO dans une image : rien n'est installe sur le Mac. Cache des toolchains dans le volume pio-cache.
FROM python:3.12-slim
RUN pip install --no-cache-dir platformio==6.1.16
WORKDIR /project
