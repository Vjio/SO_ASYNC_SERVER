# builder
FROM ubuntu:22.04 AS builder

# get dependencies
RUN apt-get update && apt-get install -y \
	build-essential \
	libaio-dev \
	&& rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY ./src ./src

# compile
RUN make -C ./src

# runner
FROM ubuntu:22.04

# only install libaio
RUN apt-get update && apt-get install -y \
	libaio1 \
	&& rm -rf /var/lib/apt/lists/*

WORKDIR /app

# get executable
COPY --from=builder /app/src/aws .

# directories expected by CI pipeline
RUN mkdir -p static dynamic
# dummy for testins
RUN echo "Hello world!" > static/index.html

EXPOSE 8888

CMD ["./aws"]
