# 发布页 / GitHub Pages

站点源码在 `site/`，无 JavaScript 构建依赖。GitHub Actions 仅上传此目录，不包含程序、缓存或本地路径。
Source lives in `site/`; no JavaScript build dependencies. The workflow uploads only this directory.

`pages.yml` 在 main 分支站点变动时自动部署，也支持手动触发。Actions 固定完整提交 SHA，部署使用独立 github-pages environment 和最小权限。
The pinned workflow deploys site changes on main or manual dispatch with scoped Pages/OIDC permissions.

规范地址 / Canonical URL: https://same.moesegfault.dev/
DNS: 将 `same` 的 CNAME 指向 `kleedaisuki.github.io`，而不是仓库路径。
DNS: point the `same` CNAME to `kleedaisuki.github.io`, not a repository path.

视觉依赖固定为 MoeSegfault Style v0.1.2，CSS 本地托管，不依赖外部 CDN 可达性。上游原始资源与 GPL 许可证保留于 `site/vendor/moesegfault-style/`，升级需核对源文件。
Style v0.1.2 is self-hosted with its upstream license, avoiding a runtime CDN dependency.

验证：检查桌面 1440px 与移动端 390px 无横向溢出，检查首页渲染、命令与版本；部署后检查 Actions 和规范地址的 HTTPS 响应。
Validation: inspect desktop/mobile layouts, commands and version; after deployment verify Actions and canonical HTTPS response.

参考 / References:
- https://docs.github.com/en/pages/getting-started-with-github-pages/using-custom-workflows-with-github-pages
- https://docs.github.com/en/pages/configuring-a-custom-domain-for-your-github-pages-site/managing-a-custom-domain-for-your-github-pages-site
- https://github.com/kleedaisuki/moesegfault-style
