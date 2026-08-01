FROM mcr.microsoft.com/devcontainers/cpp:2-debian13

# install all required dependencies.
RUN apt-get update && apt-get install -y python3 python3-pip unzip zstd curl && apt-get -y autoremove && apt-get -y autoclean


# Docker in docker setup
RUN curl -fsSL https://get.docker.com -o install-docker.sh
RUN sh install-docker.sh --version 29.6 --no-autostart

ENV DOCKER_HOST=tcp://docker:2376
ENV DOCKER_TLS_VERIFY=1
ENV DOCKER_CERT_PATH=/certs/client
# VOLUME /certs/ca
VOLUME /certs/client
