FROM ubuntu:24.04

RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        ffmpeg \
        make \
        python3 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . .
RUN make clean && make

EXPOSE 8554/tcp
ENTRYPOINT ["./rtsp-server"]
CMD ["/media", "0.0.0.0:8554"]
