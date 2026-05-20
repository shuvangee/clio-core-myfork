# Dockerfile for IOWarp Runtime container
FROM iowarp/iowarp:latest

# Copy the config file
COPY docker/test_external/clio_config_example.yaml /etc/iowarp/clio_conf.yaml
ENV CHI_SERVER_CONF=/etc/iowarp/clio_conf.yaml

# Copy the data directory
COPY context-assimilation-engine/data /workspace/context-assimilation-engine/data

WORKDIR /workspace

