# Sourced by the scripts that talk to lordos.org's AWS account (tools/web/deploy.sh,
# tools/web/cdn-apt-behaviors.sh, tools/release/apt-repo.sh; tools/web/stats.mjs reads the same file).
# The account's names and ids are not kept in the repository: they live in a private file on the
# maintainer's machines, ~/.aws/.lordos-deploy-env (LORDOS_DEPLOY_ENV overrides the path), with
# KEY=value lines:
#
#   LORDOS_AWS_PROFILE=<the AWS CLI profile that deploys the site>
#   LORDOS_S3_BUCKET=<the S3 bucket behind the site>
#   LORDOS_CF_DISTRIBUTION=<the CloudFront distribution id>
#   LORDOS_STATS_LOG_GROUP=<the CloudWatch Logs group of the visit statistics> (stats.mjs only)
#
# Only those keys are read; the file is parsed, never executed. Values already in the
# environment win over the file.
lordos_aws_env() {
  _f=${LORDOS_DEPLOY_ENV:-$HOME/.aws/.lordos-deploy-env}
  if [ -r "$_f" ]; then
    while IFS= read -r _line || [ -n "$_line" ]; do
      case $_line in
        LORDOS_AWS_PROFILE=*) [ -n "${LORDOS_AWS_PROFILE:-}" ] || LORDOS_AWS_PROFILE=${_line#*=} ;;
        LORDOS_S3_BUCKET=*) [ -n "${LORDOS_S3_BUCKET:-}" ] || LORDOS_S3_BUCKET=${_line#*=} ;;
        LORDOS_CF_DISTRIBUTION=*) [ -n "${LORDOS_CF_DISTRIBUTION:-}" ] || LORDOS_CF_DISTRIBUTION=${_line#*=} ;;
      esac
    done < "$_f"
  fi
  for _k in "$@"; do
    eval "_v=\${$_k:-}"
    [ -n "$_v" ] || { echo "$_k is not set: put it in $_f (see tools/web/aws-env.sh)" >&2; exit 1; }
  done
  unset _f _line _k _v
}
