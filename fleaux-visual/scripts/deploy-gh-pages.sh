#!/usr/bin/env bash
set -euo pipefail

show_help() {
  cat <<'EOF'
Deploy fleaux-visual to the gh-pages branch.

Usage:
  bash scripts/deploy-gh-pages.sh [--dry-run]

Options:
  --dry-run   Build and prepare the gh-pages worktree, but skip the push.

Environment:
  VITE_BASE_PATH  Optional override for Vite base path.
                  Example: /fleauxlang/ or /
EOF
}

DRY_RUN=0
if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
  show_help
  exit 0
fi
if [[ "${1:-}" == "--dry-run" ]]; then
  DRY_RUN=1
fi

REPO_ROOT="$(git rev-parse --show-toplevel)"
VISUAL_DIR="${REPO_ROOT}/fleaux-visual"

if [[ ! -d "${VISUAL_DIR}" ]]; then
  echo "error: expected visual app directory at ${VISUAL_DIR}" >&2
  exit 1
fi

if ! command -v npm >/dev/null 2>&1; then
  echo "error: npm is required" >&2
  exit 1
fi

ORIGIN_URL="$(git -C "${REPO_ROOT}" remote get-url origin)"
REPO_NAME="$(basename "${ORIGIN_URL%.git}")"

if [[ -z "${VITE_BASE_PATH:-}" ]]; then
  if [[ "${REPO_NAME}" == *.github.io ]]; then
    export VITE_BASE_PATH="/"
  else
    export VITE_BASE_PATH="/${REPO_NAME}/"
  fi
fi

echo "Using VITE_BASE_PATH=${VITE_BASE_PATH}"

echo "Building fleaux-visual..."
cd "${VISUAL_DIR}"
npm run build:pages

if [[ ! -d "${VISUAL_DIR}/dist" ]]; then
  echo "error: build output missing at ${VISUAL_DIR}/dist" >&2
  exit 1
fi

DIST_INDEX="${VISUAL_DIR}/dist/index.html"
if [[ ! -f "${DIST_INDEX}" ]]; then
  echo "error: build output missing index.html at ${DIST_INDEX}" >&2
  exit 1
fi

JS_BUNDLE="$(sed -n 's/.*src="[^"]*\/assets\/\([^"/]*\.js\)".*/\1/p' "${DIST_INDEX}" | head -n 1)"
CSS_BUNDLE="$(sed -n 's/.*href="[^"]*\/assets\/\([^"/]*\.css\)".*/\1/p' "${DIST_INDEX}" | head -n 1)"

if [[ -z "${JS_BUNDLE}" || ! -f "${VISUAL_DIR}/dist/assets/${JS_BUNDLE}" ]]; then
  echo "error: dist/index.html references a JS bundle that does not exist in dist/assets" >&2
  exit 1
fi

if [[ -z "${CSS_BUNDLE}" || ! -f "${VISUAL_DIR}/dist/assets/${CSS_BUNDLE}" ]]; then
  echo "error: dist/index.html references a CSS bundle that does not exist in dist/assets" >&2
  exit 1
fi

TMP_WORKTREE="$(mktemp -d)"
CREATED_ORPHAN_BRANCH=0
cleanup() {
  set +e
  git -C "${REPO_ROOT}" worktree remove "${TMP_WORKTREE}" --force >/dev/null 2>&1
  if [[ "${DRY_RUN}" == "1" && "${CREATED_ORPHAN_BRANCH}" == "1" ]]; then
    git -C "${REPO_ROOT}" branch -D gh-pages >/dev/null 2>&1
  fi
  rm -rf "${TMP_WORKTREE}"
}
trap cleanup EXIT

if git -C "${REPO_ROOT}" ls-remote --exit-code --heads origin gh-pages >/dev/null 2>&1; then
  git -C "${REPO_ROOT}" fetch origin gh-pages
  if git -C "${REPO_ROOT}" show-ref --verify --quiet refs/heads/gh-pages; then
    git -C "${REPO_ROOT}" branch -f gh-pages origin/gh-pages
  else
    git -C "${REPO_ROOT}" branch gh-pages origin/gh-pages
  fi
  git -C "${REPO_ROOT}" worktree add "${TMP_WORKTREE}" gh-pages
else
  git -C "${REPO_ROOT}" worktree add --detach "${TMP_WORKTREE}"
  (
    cd "${TMP_WORKTREE}"
    git checkout --orphan gh-pages
  )
  CREATED_ORPHAN_BRANCH=1
fi

rsync -a --delete --exclude '.git' "${VISUAL_DIR}/dist/" "${TMP_WORKTREE}/"
touch "${TMP_WORKTREE}/.nojekyll"

(
  cd "${TMP_WORKTREE}"
  git add -A
  if git diff --cached --quiet; then
    echo "No changes to deploy."
    exit 0
  fi

  git commit -m "Deploy fleaux-visual $(date -u +'%Y-%m-%dT%H:%M:%SZ')"

  if [[ "${DRY_RUN}" == "1" ]]; then
    echo "Dry run complete. Skipping push."
    exit 0
  fi

  git push origin gh-pages --force-with-lease
  echo "Deployment pushed to gh-pages."
)


