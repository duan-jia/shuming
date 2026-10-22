# Runtime context is .tmp/local-runtime, prepared by prepare.py.
FROM ubuntu:24.04
COPY certs/ /etc/ssl/certs/
COPY bin/ /usr/local/bin/
COPY lib/ /opt/shuming/lib/
ENV LD_LIBRARY_PATH=/opt/shuming/lib
WORKDIR /app
USER 1000:1000
EXPOSE 9006
ENTRYPOINT ["/usr/local/bin/fast-llm-reasoner"]
