import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'
import tailwindcss from '@tailwindcss/vite'

function normalizeBasePath(rawBasePath: string | undefined): string {
  if (!rawBasePath || rawBasePath.trim().length === 0) {
    return '/'
  }

  const trimmed = rawBasePath.trim()
  const withLeadingSlash = trimmed.startsWith('/') ? trimmed : `/${trimmed}`
  return withLeadingSlash.endsWith('/') ? withLeadingSlash : `${withLeadingSlash}/`
}

function detectGitHubPagesBasePath(): string {
  const repositorySlug = process.env.GITHUB_REPOSITORY
  const repositoryName = repositorySlug?.split('/')[1]

  if (!repositoryName) {
    return '/'
  }

  if (repositoryName.endsWith('.github.io')) {
    return '/'
  }

  return `/${repositoryName}/`
}

// https://vite.dev/config/
export default defineConfig({
  base: normalizeBasePath(process.env.VITE_BASE_PATH ?? (process.env.GITHUB_ACTIONS ? detectGitHubPagesBasePath() : '/')),
  plugins: [react(), tailwindcss()],
})
