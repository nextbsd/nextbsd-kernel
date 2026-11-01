/*
 * NextBSD IOMedia matching (nextbsd-kernel#364): GEOM layers load the way
 * CoreStorage and AppleRAID do on Darwin.
 *
 * On Darwin every partition is an IOMedia whose content hint is its GPT type
 * GUID, and a layer's kext carries a personality on it (IOProviderClass IOMedia
 * + Content Mask); IOKit loads the kext when the media appears, the layer
 * publishes the assembled or decrypted disk as new media, and DiskArbitration
 * mounts it. No daemon decides.
 *
 * NextBSD's disks are GEOM providers and its layers are GEOM classes shipped
 * as kexts (GeomEli, GeomMirror, ...). This file is a GEOM class that never
 * attaches: its taste method reads what a provider carries, as the layer's own
 * taste would, and offers it to the in-kernel IOCatalogue matcher
 * (iocat_media_offer() in iokit_catalogue.c):
 *
 *  - the GEOM metadata in the provider's last sector, by its magic
 *    ("GEOM::ELI", "GEOM::MIRROR", "GEOM::STRIPE", ...): FreeBSD's layers keep
 *    no partition type of their own, they keep this; and
 *  - for a GPT partition, its type GUID in Darwin's form (upper case), so a
 *    personality can name a partition type as CoreStorage's does.
 *
 * A personality that claims the content gets a kextd load request, as for any
 * device. Loading the kext registers its GEOM class, which tastes every
 * existing provider (g_load_class), finds its metadata, and publishes the new
 * provider, which this class tastes in turn (a mirror of geli providers works).
 *
 * Metadata whose class is already loaded is not offered: that class has
 * already tasted the provider, and the offer would only repeat a load.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/malloc.h>
#include <sys/sbuf.h>
#include <sys/ctype.h>
#include <sys/iocatalogue.h>

#include <geom/geom.h>
#include <geom/geom_int.h>	/* g_classes */

#define	IOMEDIA_CLASS_NAME	"IOMEDIA"
#define	IOMEDIA_MAGIC_PREFIX	"GEOM::"
#define	IOMEDIA_MAGIC_LEN	16	/* md_magic[16] in every GEOM layer */
#define	IOMEDIA_GUID_LEN	36

static g_taste_t iomedia_taste;

static struct g_class iomedia_class = {
	.name = IOMEDIA_CLASS_NAME,
	.version = G_VERSION,
	.taste = iomedia_taste,
};

/* Our taste geom lives within one event: no orphan or spoil can reach it. */
static void
iomedia_orphan(struct g_consumer *cp __unused)
{

	KASSERT(0, ("%s called", __func__));
}

/* True when a GEOM class called `name` is registered. Topology lock held. */
static bool
iomedia_class_loaded(const char *name)
{
	struct g_class *mp;

	g_topology_assert();
	LIST_FOREACH(mp, &g_classes, class) {
		if (strcmp(mp->name, name) == 0)
			return (true);
	}
	return (false);
}

/*
 * The GEOM magic in the provider's last sector, if any, into `magic`
 * (IOMEDIA_MAGIC_LEN + 1). Every GEOM layer that keeps on-disk metadata keeps
 * it there, md_magic first ("GEOM::ELI\0...").
 */
static void
iomedia_read_magic(struct g_class *mp, struct g_provider *pp, char *magic)
{
	struct g_geom *gp;
	struct g_consumer *cp;
	u_char *buf;
	size_t n, i;
	int error;

	g_topology_assert();
	magic[0] = '\0';
	if (pp->sectorsize < IOMEDIA_MAGIC_LEN ||
	    pp->mediasize < (off_t)pp->sectorsize)
		return;

	gp = g_new_geomf(mp, "iomedia:taste");
	gp->orphan = iomedia_orphan;
	cp = g_new_consumer(gp);
	cp->flags |= G_CF_DIRECT_SEND | G_CF_DIRECT_RECEIVE;
	buf = NULL;
	if (g_attach(cp, pp) != 0)
		goto out;
	if (g_access(cp, 1, 0, 0) != 0)
		goto out;
	g_topology_unlock();
	buf = g_read_data(cp, pp->mediasize - pp->sectorsize, pp->sectorsize,
	    &error);
	g_topology_lock();
	if (buf == NULL)
		goto out;
	if (memcmp(buf, IOMEDIA_MAGIC_PREFIX,
	    sizeof(IOMEDIA_MAGIC_PREFIX) - 1) != 0)
		goto out;
	n = strnlen((const char *)buf, IOMEDIA_MAGIC_LEN);
	if (n == IOMEDIA_MAGIC_LEN || n == sizeof(IOMEDIA_MAGIC_PREFIX) - 1)
		goto out;
	/* A class name: upper case and digits ("RAID3"), nothing else. */
	for (i = sizeof(IOMEDIA_MAGIC_PREFIX) - 1; i < n; i++) {
		if (!((buf[i] >= 'A' && buf[i] <= 'Z') ||
		    (buf[i] >= '0' && buf[i] <= '9')))
			goto out;
	}
	memcpy(magic, buf, n);
	magic[n] = '\0';
out:
	g_free(buf);
	if (cp->provider != NULL) {
		if (cp->acr == 1)
			g_access(cp, -1, 0, 0);
		g_detach(cp);
	}
	g_destroy_consumer(cp);
	g_destroy_geom(gp);
}

/*
 * A GPT partition's type GUID, upper case, into `guid` (IOMEDIA_GUID_LEN + 1).
 * g_part keeps the GUID private to its GPT scheme; its conftxt line (the
 * libdisk form "... xs GPT xt <guid>", the one kern.geom.conftxt shows) is
 * the scheme's own description of it.
 */
static void
iomedia_gpt_type(struct g_provider *pp, char *guid)
{
	struct g_geom *gp;
	struct sbuf *sb;
	const char *p;
	size_t i;

	g_topology_assert();
	guid[0] = '\0';
	gp = pp->geom;
	if (strcmp(gp->class->name, "PART") != 0 || gp->dumpconf == NULL ||
	    pp->private == NULL)
		return;
	sb = sbuf_new_auto();
	gp->dumpconf(sb, NULL, gp, NULL, pp);
	if (sbuf_finish(sb) == 0 &&
	    (p = strstr(sbuf_data(sb), " xs GPT xt ")) != NULL) {
		p += strlen(" xs GPT xt ");
		for (i = 0; i < IOMEDIA_GUID_LEN; i++) {
			if (p[i] == '\0')
				break;
			guid[i] = toupper((unsigned char)p[i]);
		}
		guid[i] = '\0';
		if (i != IOMEDIA_GUID_LEN || guid[8] != '-' ||
		    guid[13] != '-' || guid[18] != '-' || guid[23] != '-')
			guid[0] = '\0';
	}
	sbuf_delete(sb);
}

static struct g_geom *
iomedia_taste(struct g_class *mp, struct g_provider *pp, int flags __unused)
{
	char magic[IOMEDIA_MAGIC_LEN + 1];
	char guid[IOMEDIA_GUID_LEN + 1];

	g_topology_assert();
	g_trace(G_T_TOPOLOGY, "iomedia_taste(%s, %s)", mp->name, pp->name);

	iomedia_gpt_type(pp, guid);
	iomedia_read_magic(mp, pp, magic);
	if (magic[0] != '\0' &&
	    iomedia_class_loaded(magic + sizeof(IOMEDIA_MAGIC_PREFIX) - 1))
		magic[0] = '\0';
	if (magic[0] == '\0' && guid[0] == '\0')
		return (NULL);

	/* Offering may sleep (malloc); taste already does (g_read_data). */
	iocat_media_offer(pp->name, magic, guid);
	return (NULL);
}

DECLARE_GEOM_CLASS(iomedia_class, g_iomedia);
MODULE_VERSION(geom_iomedia, 1);
