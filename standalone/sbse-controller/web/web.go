// Package web embeds the built dashboard (web/dist) into the binary. The dist
// directory is produced by `npm run build`; a placeholder index.html is committed
// so the Go build succeeds before the frontend is built.
package web

import (
	"embed"
	"io/fs"
)

//go:embed all:dist
var dist embed.FS

// FS returns the dashboard file system rooted at dist/.
func FS() fs.FS {
	sub, err := fs.Sub(dist, "dist")
	if err != nil {
		panic(err) // dist is embedded at build time; this cannot fail at runtime
	}
	return sub
}
