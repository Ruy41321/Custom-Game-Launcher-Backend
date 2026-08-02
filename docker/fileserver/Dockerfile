FROM nginx:1.27-alpine

# The official image runs envsubst over /etc/nginx/templates/*.template at start-up,
# substituting only the variables listed in NGINX_ENVSUBST_FILTER. nginx's own runtime
# variables ($uri, $secure_link, ...) are therefore left untouched.
ENV NGINX_ENVSUBST_FILTER="FILE_SECURE_LINK_SECRET"

COPY templates/default.conf.template /etc/nginx/templates/default.conf.template

HEALTHCHECK --interval=15s --timeout=5s --retries=3 \
    CMD wget -q -O /dev/null http://127.0.0.1/healthz || exit 1
