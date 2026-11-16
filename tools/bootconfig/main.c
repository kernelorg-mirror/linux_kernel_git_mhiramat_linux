// SPDX-License-Identifier: GPL-2.0
/*
 * Boot config tool for initrd image
 */
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <endian.h>
#include <assert.h>

#include <linux/bootconfig.h>

static void show_xbc_error(const char *data, const char *msg, int pos)
{
	int lin = 1, col, i;

	if (pos < 0 || !data) {
		pr_err("Error: %s.\n", msg);
		return;
	}

	/* Note that pos starts from 0 but lin and col should start from 1. */
	col = pos + 1;
	for (i = 0; i < pos; i++) {
		if (data[i] == '\n') {
			lin++;
			col = pos - i;
		}
	}
	pr_err("Parse Error: %s at %d:%d\n", msg, lin, col);

}

static int init_xbc_with_error(const char *buf, int len)
{
	const char *msg;
	int ret, pos;

	ret = xbc_init(buf, len, &msg, &pos);
	if (ret < 0)
		show_xbc_error(buf, msg, pos);

	return ret;
}

static int xbc_show_value(struct xbc_node *node, bool semicolon)
{
	const char *val, *eol;
	char q;
	int i = 0;

	eol = semicolon ? ";\n" : "\n";
	xbc_array_for_each_value(node, val) {
		if (strchr(val, '"'))
			q = '\'';
		else
			q = '"';
		printf("%c%s%c%s", q, val, q, xbc_node_is_array(node) ? ", " : eol);
		i++;
	}
	return i;
}

/* Match the conditions under which the compact formatter opens braces. */
static bool xbc_node_needs_braces(struct xbc_node *node)
{
	struct xbc_node *child = xbc_node_get_child(node);

	if (child && xbc_node_is_value(child))
		child = xbc_node_get_next(child);
	if (!child)
		return false;
	if (child->next)
		return true;

	/* A single subkey with both a value and subkeys is not compacted. */
	child = xbc_node_get_child(child);
	return child && xbc_node_is_value(child) && child->next;
}

static void xbc_show_compact_tree(void)
{
	struct xbc_node *node, *cnode = NULL, *vnode;
	int depth = 0, i;

	node = xbc_root_node();
	while (node && xbc_node_is_key(node)) {
		for (i = 0; i < depth; i++)
			printf("\t");
		if (!cnode)
			cnode = xbc_node_get_child(node);
		while (cnode && xbc_node_is_key(cnode) && !cnode->next) {
			vnode = xbc_node_get_child(cnode);
			/*
			 * If @cnode has value and subkeys, this
			 * should show it as below.
			 *
			 * key(@node) {
			 *      key(@cnode) = value;
			 *      key(@cnode) {
			 *          subkeys;
			 *      }
			 * }
			 */
			if (vnode && xbc_node_is_value(vnode) && vnode->next)
				break;
			printf("%s.", xbc_node_get_data(node));
			node = cnode;
			cnode = vnode;
		}
		if (cnode && xbc_node_is_key(cnode)) {
			printf("%s {\n", xbc_node_get_data(node));
			depth++;
			node = cnode;
			cnode = NULL;
			continue;
		} else if (cnode && xbc_node_is_value(cnode)) {
			printf("%s = ", xbc_node_get_data(node));
			xbc_show_value(cnode, true);
			/*
			 * If @node has value and subkeys, continue
			 * looping on subkeys with same node.
			 */
			if (cnode->next) {
				cnode = xbc_node_get_next(cnode);
				continue;
			}
		} else {
			printf("%s;\n", xbc_node_get_data(node));
		}
		cnode = NULL;

		if (node->next) {
			node = xbc_node_get_next(node);
			continue;
		}
		while (!node->next) {
			node = xbc_node_get_parent(node);
			if (!node)
				return;
			if (!depth || !xbc_node_needs_braces(node))
				continue;
			depth--;
			for (i = 0; i < depth; i++)
				printf("\t");
			printf("}\n");
		}
		node = xbc_node_get_next(node);
	}
}

static void xbc_show_list(void)
{
	char key[XBC_KEYLEN_MAX];
	struct xbc_node *leaf;
	const char *val;
	int ret;

	xbc_for_each_key_value(leaf, val) {
		ret = xbc_node_compose_key(leaf, key, XBC_KEYLEN_MAX);
		if (ret < 0) {
			fprintf(stderr, "Failed to compose key %d\n", ret);
			break;
		}
		printf("%s = ", key);
		if (!val || val[0] == '\0') {
			printf("\"\"\n");
			continue;
		}
		xbc_show_value(xbc_node_get_child(leaf), false);
	}
}

static int load_xbc_fd(int fd, char **buf, int size)
{
	int ret;

	if (size < 0 || size > XBC_DATA_MAX)
		return -EINVAL;

	*buf = malloc(size + 1);
	if (!*buf)
		return -ENOMEM;

	ret = read(fd, *buf, size);
	if (ret < 0)
		return -errno;
	(*buf)[size] = '\0';

	return ret;
}

/* Return the read size or -errno */
static int load_xbc_file(const char *path, char **buf)
{
	struct stat stat;
	int fd, ret;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -errno;
	ret = fstat(fd, &stat);
	if (ret < 0) {
		ret = -errno;
		close(fd);
		return ret;
	}

	if (stat.st_size > XBC_DATA_MAX) {
		pr_err("%s size is too big\n", path);
		ret = -E2BIG;
		close(fd);
		return ret;
	}

	ret = load_xbc_fd(fd, buf, stat.st_size);

	close(fd);

	return ret;
}

static int pr_errno(const char *msg, int err)
{
	pr_err("%s: %d\n", msg, err);
	return err;
}

struct xbc_block {
	struct xbc_config cfg;
	char *raw_buf;
};

static void free_xbc_blocks(struct xbc_block *blocks, int count)
{
	int i;

	for (i = 0; i < count; i++) {
		free(blocks[i].raw_buf);
		blocks[i].raw_buf = NULL;
	}
}

static int load_xbc_list_from_initrd(int fd, struct xbc_block *blocks,
				     int max_blocks, off_t *base_size)
{
	struct stat stat;
	int ret, total = 0, i;
	uint32_t size = 0, csum = 0, rcsum;
	char magic[BOOTCONFIG_MAGIC_LEN];
	char *buf = NULL;
	off_t end;

	memset(blocks, 0, sizeof(*blocks) * max_blocks);

	ret = fstat(fd, &stat);
	if (ret < 0)
		return -errno;

	end = stat.st_size;
	while (end >= (off_t)BOOTCONFIG_FOOTER_SIZE) {
		if (lseek(fd, end - (off_t)BOOTCONFIG_MAGIC_LEN, SEEK_SET) < 0) {
			ret = pr_errno("Failed to lseek for magic", -errno);
			goto err;
		}
		if (read(fd, magic, BOOTCONFIG_MAGIC_LEN) != BOOTCONFIG_MAGIC_LEN) {
			ret = pr_errno("Failed to read", -errno);
			goto err;
		}

		/* Check the bootconfig magic bytes */
		if (memcmp(magic, BOOTCONFIG_MAGIC, BOOTCONFIG_MAGIC_LEN) != 0)
			break;

		if (lseek(fd, end - (off_t)BOOTCONFIG_FOOTER_SIZE, SEEK_SET) < 0) {
			ret = pr_errno("Failed to lseek for size", -errno);
			goto err;
		}
		if (read(fd, &size, sizeof(uint32_t)) != sizeof(uint32_t)) {
			ret = pr_errno("Failed to read size", -errno);
			goto err;
		}
		size = le32toh(size);

		if (read(fd, &csum, sizeof(uint32_t)) != sizeof(uint32_t)) {
			ret = pr_errno("Failed to read checksum", -errno);
			goto err;
		}
		csum = le32toh(csum);

		/* Wrong size error */
		if (size > XBC_DATA_MAX ||
		    (off_t)size > end - (off_t)BOOTCONFIG_FOOTER_SIZE) {
			pr_err("bootconfig size is too big\n");
			ret = -E2BIG;
			goto err;
		}

		if (lseek(fd, end - (off_t)(size + BOOTCONFIG_FOOTER_SIZE),
			  SEEK_SET) < 0) {
			ret = pr_errno("Failed to lseek", -errno);
			goto err;
		}

		ret = load_xbc_fd(fd, &buf, size);
		if (ret < 0)
			goto err;

		/* Wrong Checksum */
		rcsum = xbc_calc_checksum(buf, size);
		if (csum != rcsum) {
			pr_err("checksum error: %u != %u\n", csum, rcsum);
			free(buf);
			ret = -EINVAL;
			goto err;
		}

		total++;
		if (total > max_blocks) {
			pr_err("Too many bootconfigs in initrd (max %d)\n",
			       max_blocks);
			free(buf);
			ret = -E2BIG;
			goto err;
		}

		if (xbc_parse_id_header(buf, size,
					&blocks[max_blocks - total].cfg) < 0) {
			pr_err("Invalid bootconfig ID header\n");
			free(buf);
			ret = -EINVAL;
			goto err;
		}
		blocks[max_blocks - total].raw_buf = buf;
		end -= (off_t)(size + BOOTCONFIG_FOOTER_SIZE);
	}

	if (total > 0 && total < max_blocks)
		memmove(blocks, &blocks[max_blocks - total],
			total * sizeof(*blocks));

	for (i = 0; i < total; i++) {
		int j;

		if (!blocks[i].cfg.name[0])
			continue;
		for (j = i + 1; j < total; j++) {
			if (!strcmp(blocks[i].cfg.name, blocks[j].cfg.name)) {
				pr_err("Duplicate bootconfig ID '%s' in initrd\n",
				       blocks[i].cfg.name);
				ret = -EINVAL;
				goto err_all;
			}
		}
	}

	if (base_size)
		*base_size = end;

	return total;

err:
	if (total > 0) {
		int n = total > max_blocks ? max_blocks : total;

		free_xbc_blocks(&blocks[max_blocks - n], n);
	}
	return ret;

err_all:
	free_xbc_blocks(blocks, total);
	return ret;
}

static int show_xbc_kernel_cmdline(void)
{
	struct xbc_node *root;
	char *buf = NULL;
	int len, ret;

	root = xbc_find_node("kernel");
	if (!root)
		return 0;	/* no kernel.* keys: emit empty output */

	len = xbc_snprint_cmdline(NULL, 0, root);
	if (len < 0) {
		pr_err("Failed to size cmdline output: %d\n", len);
		return len;
	}
	if (len == 0)
		return 0;

	buf = malloc(len + 1);
	if (!buf)
		return -ENOMEM;

	ret = xbc_snprint_cmdline(buf, len + 1, root);
	if (ret < 0) {
		pr_err("Failed to render cmdline output: %d\n", ret);
		free(buf);
		return ret;
	}

	fputs(buf, stdout);
	free(buf);
	return 0;
}

static int render_xbc_output(bool list, bool render_cmdline)
{
	if (render_cmdline)
		return show_xbc_kernel_cmdline();
	if (list)
		xbc_show_list();
	else
		xbc_show_compact_tree();
	return 0;
}

static int show_xbc(const char *path, bool list, bool render_cmdline,
		    const char *id_spec)
{
	struct xbc_block blocks[XBC_CONFIG_MAX];
	struct xbc_config cfgs[XBC_CONFIG_MAX];
	int ret, fd, count, i, err_idx = -1, pos;
	const char *msg;
	char *buf = NULL;
	struct stat st;
	bool seeded = false;

	ret = stat(path, &st);
	if (ret < 0) {
		ret = -errno;
		pr_err("Failed to stat %s: %d\n", path, ret);
		return ret;
	}

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		ret = -errno;
		pr_err("Failed to open initrd %s: %d\n", path, ret);
		return ret;
	}

	count = load_xbc_list_from_initrd(fd, blocks, XBC_CONFIG_MAX, NULL);
	close(fd);
	if (count < 0) {
		pr_err("Failed to load a boot config from initrd: %d\n", count);
		return count;
	}

	/* Assume a bootconfig file if no trailer and small enough */
	if (count == 0) {
		if (id_spec) {
			pr_err("No bootconfig found in %s\n", path);
			return -ENOENT;
		}
		if (st.st_size <= XBC_DATA_MAX) {
			int len = load_xbc_file(path, &buf);

			if (len < 0) {
				pr_err("Failed to load a boot config: %d\n", len);
				return len;
			}
			if (init_xbc_with_error(buf, len) >= 0) {
				ret = render_xbc_output(list, render_cmdline);
				xbc_exit();
			} else {
				ret = len ? -EINVAL : 0;
			}
			free(buf);
			return ret > 0 ? 0 : ret;
		}
		return render_xbc_output(list, render_cmdline);
	}

	for (i = 0; i < count; i++)
		cfgs[i] = blocks[i].cfg;

	if (id_spec) {
		bool any_enabled = false;

		if (*id_spec == '\0') {
			pr_err("Error: Empty bootconfig ID specification.\n");
			ret = -EINVAL;
			goto out_blocks;
		}
		ret = xbc_enable_configs(cfgs, count, id_spec, &seeded);
		if (ret < 0)
			goto out_blocks;
		for (i = 0; i < count; i++) {
			if (cfgs[i].enabled) {
				any_enabled = true;
				break;
			}
		}
		if (!any_enabled) {
			ret = -ENOENT;
			goto out_blocks;
		}
		ret = xbc_init_configs(cfgs, count, &msg, &pos, &err_idx);
		if (ret < 0) {
			if (err_idx >= 0)
				pr_err("Error in bootconfig [%d%s%s]:\n",
				       err_idx,
				       cfgs[err_idx].name[0] ? ":" : "",
				       cfgs[err_idx].name);
			show_xbc_error(err_idx >= 0 ? cfgs[err_idx].data : NULL,
				       msg, pos);
			goto out_blocks;
		}
		ret = render_xbc_output(list, render_cmdline);
		xbc_exit();
	} else if (render_cmdline) {
		cfgs[0].enabled = true;
		ret = xbc_init_configs(cfgs, count, &msg, &pos, &err_idx);
		if (ret < 0) {
			show_xbc_error(err_idx >= 0 ? cfgs[err_idx].data : NULL,
				       msg, pos);
			goto out_blocks;
		}
		ret = show_xbc_kernel_cmdline();
		xbc_exit();
	} else if (count == 1 && !cfgs[0].name[0]) {
		ret = init_xbc_with_error(cfgs[0].data, cfgs[0].size);
		if (ret >= 0) {
			ret = render_xbc_output(list, false);
			xbc_exit();
		}
	} else {
		for (i = 0; i < count; i++) {
			if (cfgs[i].name[0])
				printf("# [%d] #BOOTCONFIG-ID: %s\n",
				       i, cfgs[i].name);
			else
				printf("# [%d] (default)\n", i);
			ret = init_xbc_with_error(cfgs[i].data, cfgs[i].size);
			if (ret < 0)
				goto out_blocks;
			ret = render_xbc_output(list, false);
			xbc_exit();
			if (ret < 0)
				goto out_blocks;
		}
	}

out_blocks:
	free_xbc_blocks(blocks, count);
	return ret > 0 ? 0 : ret;
}

static int write_xbc_blocks(int fd, off_t base_size, struct xbc_block *blocks,
			    int count, const char *path)
{
	struct {
		uint32_t size;
		uint32_t csum;
		char magic[BOOTCONFIG_MAGIC_LEN];
	} footer;
	off_t cur_off = base_size;
	int i, ret = 0;

	static_assert(sizeof(footer) == BOOTCONFIG_FOOTER_SIZE,
		      "bootconfig footer size mismatch");

	if (ftruncate(fd, base_size) < 0)
		return pr_errno("Failed to truncate initrd", -errno);

	if (lseek(fd, base_size, SEEK_SET) < 0)
		return pr_errno("Failed to lseek initrd", -errno);

	for (i = 0; i < count; i++) {
		char hdr[BOOTCONFIG_ID_PREFIX_LEN + BOOTCONFIG_ID_MAX + 2];
		size_t hdr_len = 0, body_len = blocks[i].cfg.size;
		size_t unpadded, total_size, padded, write_size;
		uint32_t csum;
		char *data;
		int pad;

		if (blocks[i].cfg.name[0])
			hdr_len = snprintf(hdr, sizeof(hdr), "%s%s\n",
					   BOOTCONFIG_ID_PREFIX,
					   blocks[i].cfg.name);

		unpadded = hdr_len + body_len;
		if (body_len == 0 || blocks[i].cfg.data[body_len - 1] != '\0')
			unpadded++;

		data = calloc(unpadded + BOOTCONFIG_ALIGN + BOOTCONFIG_FOOTER_SIZE, 1);
		if (!data) {
			ret = -ENOMEM;
			goto out_rollback;
		}
		if (hdr_len)
			memcpy(data, hdr, hdr_len);
		if (body_len)
			memcpy(data + hdr_len, blocks[i].cfg.data, body_len);

		csum = xbc_calc_checksum(data, unpadded);

		total_size = (size_t)cur_off + unpadded + BOOTCONFIG_FOOTER_SIZE;
		pad = ((total_size + BOOTCONFIG_ALIGN - 1) & (~BOOTCONFIG_ALIGN_MASK)) - total_size;
		padded = unpadded + pad;

		footer.size = htole32((uint32_t)padded);
		footer.csum = htole32(csum);
		memcpy(footer.magic, BOOTCONFIG_MAGIC, BOOTCONFIG_MAGIC_LEN);
		memcpy(data + padded, &footer, BOOTCONFIG_FOOTER_SIZE);

		write_size = padded + BOOTCONFIG_FOOTER_SIZE;
		ret = write(fd, data, write_size);
		free(data);
		if (ret < (ssize_t)write_size) {
			if (ret < 0)
				ret = -errno;
			else
				ret = -ENOSPC;
			pr_err("Failed to apply a boot config: %d\n", ret);
			goto out_rollback;
		}
		cur_off += (off_t)write_size;
	}

	return 0;

out_rollback:
	if (ftruncate(fd, base_size) < 0) {
		int rerr = -errno;

		pr_err("Failed to rollback the write error: %d\n", rerr);
		pr_err("The initrd %s may be corrupted. Recommend to rebuild.\n", path);
	}
	return ret;
}

static int delete_xbc(const char *path, const char *id_spec)
{
	struct xbc_block blocks[XBC_CONFIG_MAX];
	struct xbc_block kept[XBC_CONFIG_MAX];
	struct xbc_config cfgs[XBC_CONFIG_MAX];
	off_t base_size = 0;
	int ret = 0, fd, count, kept_count = 0, i;
	bool seeded = false;

	if (id_spec && (*id_spec == '\0' || *id_spec == '+')) {
		pr_err("Invalid delete ID specification '%s'\n", id_spec);
		return -EINVAL;
	}

	fd = open(path, O_RDWR);
	if (fd < 0) {
		ret = -errno;
		pr_err("Failed to open initrd %s: %d\n", path, ret);
		return ret;
	}

	count = load_xbc_list_from_initrd(fd, blocks, XBC_CONFIG_MAX, &base_size);
	if (count < 0) {
		ret = count;
		pr_err("Failed to load a boot config from initrd: %d\n", ret);
		goto out;
	}

	if (count == 0) {
		if (id_spec) {
			pr_err("No bootconfig found in %s\n", path);
			ret = -ENOENT;
		}
		goto out;
	}

	if (!id_spec) {
		if (ftruncate(fd, base_size) < 0)
			ret = -errno;
		goto out_free;
	}

	for (i = 0; i < count; i++)
		cfgs[i] = blocks[i].cfg;

	ret = xbc_enable_configs(cfgs, count, id_spec, &seeded);
	if (ret < 0)
		goto out_free;

	for (i = 0; i < count; i++) {
		if (!cfgs[i].enabled)
			kept[kept_count++] = blocks[i];
	}

	ret = write_xbc_blocks(fd, base_size, kept, kept_count, path);

out_free:
	free_xbc_blocks(blocks, count);
out:
	close(fd);
	return ret;
}

static int apply_xbc(const char *path, const char *xbc_path, const char *id)
{
	struct xbc_block blocks[XBC_CONFIG_MAX];
	char *buf = NULL;
	off_t base_size = 0;
	size_t hdr_len = 0, body_size, block_size;
	uint32_t csum;
	int ret, fd, count, i, target = -1;

	if (id && !xbc_valid_id(id, strlen(id))) {
		pr_err("Invalid bootconfig ID '%s'\n", id);
		return -EINVAL;
	}

	ret = load_xbc_file(xbc_path, &buf);
	if (ret < 0) {
		pr_err("Failed to load %s : %d\n", xbc_path, ret);
		return ret;
	}
	body_size = ret;
	if (body_size >= BOOTCONFIG_ID_PREFIX_LEN &&
	    !memcmp(buf, BOOTCONFIG_ID_PREFIX, BOOTCONFIG_ID_PREFIX_LEN)) {
		pr_err("Input file %s must not contain %s header; use -i <id>\n",
		       xbc_path, BOOTCONFIG_ID_PREFIX);
		free(buf);
		return -EINVAL;
	}

	if (body_size == 0 || buf[body_size - 1] != '\0')
		body_size++;

	if (id)
		hdr_len = BOOTCONFIG_ID_PREFIX_LEN + strlen(id) + 1;
	block_size = hdr_len + body_size;
	if (block_size > XBC_DATA_MAX) {
		pr_err("%s size is too big with ID header\n", xbc_path);
		free(buf);
		return -E2BIG;
	}

	/* Verify the data format standalone */
	ret = init_xbc_with_error(buf, body_size);
	if (ret < 0) {
		free(buf);
		return ret;
	}

	csum = xbc_calc_checksum(buf, body_size);
	if (id) {
		char hdr[BOOTCONFIG_ID_PREFIX_LEN + BOOTCONFIG_ID_MAX + 2];

		snprintf(hdr, sizeof(hdr), "%s%s\n", BOOTCONFIG_ID_PREFIX, id);
		csum += xbc_calc_checksum(hdr, hdr_len);
	}

	printf("Apply %s to %s\n", xbc_path, path);
	if (id)
		printf("\tID: %s\n", id);
	xbc_get_info(&ret, NULL);
	printf("\tNumber of nodes: %d\n", ret);
	printf("\tSize: %zu bytes\n", block_size);
	printf("\tChecksum: %u\n", (unsigned int)csum);

	/* TODO: Check the options by schema */
	xbc_exit();

	fd = open(path, O_RDWR);
	if (fd < 0) {
		ret = -errno;
		pr_err("Failed to open %s: %d\n", path, ret);
		free(buf);
		return ret;
	}

	count = load_xbc_list_from_initrd(fd, blocks, XBC_CONFIG_MAX, &base_size);
	if (count < 0) {
		ret = count;
		pr_err("Failed to load existing boot configs: %d\n", ret);
		free(buf);
		close(fd);
		return ret;
	}

	if (!id) {
		for (i = 0; i < count; i++) {
			if (!blocks[i].cfg.name[0]) {
				target = i;
				break;
			}
		}
		if (target >= 0) {
			free(blocks[target].raw_buf);
		} else {
			if (count >= XBC_CONFIG_MAX) {
				pr_err("Too many bootconfigs in %s (max %d)\n",
				       path, XBC_CONFIG_MAX);
				ret = -E2BIG;
				goto out_err;
			}
			if (count > 0)
				memmove(&blocks[1], &blocks[0],
					count * sizeof(*blocks));
			target = 0;
			count++;
		}
		blocks[target].cfg.name = "";
	} else {
		for (i = 0; i < count; i++) {
			if (!strcmp(blocks[i].cfg.name, id)) {
				target = i;
				break;
			}
		}
		if (target >= 0) {
			free(blocks[target].raw_buf);
		} else {
			if (count >= XBC_CONFIG_MAX) {
				pr_err("Too many bootconfigs in %s (max %d)\n",
				       path, XBC_CONFIG_MAX);
				ret = -E2BIG;
				goto out_err;
			}
			target = count++;
		}
		blocks[target].cfg.name = id;
	}

	blocks[target].raw_buf = buf;
	blocks[target].cfg.data = buf;
	blocks[target].cfg.size = body_size;
	blocks[target].cfg.enabled = false;
	buf = NULL;

	ret = write_xbc_blocks(fd, base_size, blocks, count, path);

out_err:
	free(buf);
	free_xbc_blocks(blocks, count);
	close(fd);
	return ret;
}

static int usage(void)
{
	printf("Usage: bootconfig [OPTIONS] <INITRD>\n"
		"Or     bootconfig <CONFIG>\n"
		" Apply, delete or show boot config to initrd.\n"
		" Options:\n"
		"		-a <config>: Apply boot config to initrd\n"
		"		-d : Delete boot config file from initrd\n"
		"		-i <id[,...]> : Specify boot config ID or index\n"
		"		-l : list boot config in initrd or file\n"
		"		-C : render the kernel.* subtree as a flat cmdline\n"
		"		     string (suitable for embedding in a kernel image)\n"
		"		     and print it to stdout\n\n"
		" If no option is given, show the bootconfig in the given file.\n");
	return -1;
}

int main(int argc, char **argv)
{
	char *path = NULL;
	char *apply = NULL;
	char *id = NULL;
	bool render_cmdline = false;
	bool delete = false, list = false;
	int opt;

	while ((opt = getopt(argc, argv, "hda:i:lC")) != -1) {
		switch (opt) {
		case 'd':
			delete = true;
			break;
		case 'a':
			apply = optarg;
			break;
		case 'i':
			id = optarg;
			break;
		case 'l':
			list = true;
			break;
		case 'C':
			render_cmdline = true;
			break;
		case 'h':
		default:
			return usage();
		}
	}

	if ((!!apply + !!delete + !!list + !!render_cmdline) > 1) {
		pr_err("Error: You can give one of -a, -d, -l or -C at once.\n");
		return usage();
	}

	if (optind >= argc) {
		pr_err("Error: No initrd is specified.\n");
		return usage();
	}

	path = argv[optind];

	if (apply)
		return apply_xbc(path, apply, id);
	else if (delete)
		return delete_xbc(path, id);

	return show_xbc(path, list, render_cmdline, id);
}
