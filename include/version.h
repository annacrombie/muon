#ifndef MUON_VERSION_H
#define MUON_VERSION_H
struct muon_version {
	const char *const version, *const vcs_tag, *const meson_compat;
};
extern const struct muon_version muon_version;

#endif
