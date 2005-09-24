#include "nutmerge.h"
#include "avireader.h"

static int mk_riff_tree(FILE * in, riff_tree_t * tree) {
	int left;
	tree->tree = NULL;
	tree->data = NULL;
	tree->amount = 0;
	tree->offset = ftell(in);
	FREAD(in, 4, tree->name);
	FREAD(in, 4, &tree->len);
	FIXENDIAN32(tree->len);
	left = tree->len;

	switch(strFOURCC(tree->name)) {
		case mmioFOURCC('L','I','S','T'):
		case mmioFOURCC('R','I','F','F'):
			tree->type = 0;
			FREAD(in, 4, tree->listname); left -= 4; // read real name
			if (!strncmp(tree->listname, "movi", 4)) {
				fseek(in, left, SEEK_CUR);
				break;
			}
			while (left > 0) {
				int err;
				tree->tree =
					realloc(tree->tree, sizeof(riff_tree_t) * ++tree->amount);
				if ((err = mk_riff_tree(in, &tree->tree[tree->amount - 1])))
					return err;
				left -= (tree->tree[tree->amount - 1].len + 8);
				if (tree->tree[tree->amount - 1].len & 1) left--;
			}
			break;
		default:
			tree->type = 1;
			tree->data = malloc(left);
			FREAD(in, left, tree->data);
	}
	if (tree->len & 1) fgetc(in);
	return 0;
}

void free_riff_tree(riff_tree_t * tree) {
	int i;
	if (!tree) return;

	for (i = 0; i < tree->amount; i++) free_riff_tree(&tree->tree[i]);
	tree->amount = 0;

	free(tree->tree); tree->tree = NULL;
	free(tree->data); tree->data = NULL;
}

full_riff_tree_t * init_riff() {
	full_riff_tree_t * full = malloc(sizeof(full_riff_tree_t));
	full->amount = 0;
	full->tree = NULL;
	return full;
}

int get_full_riff_tree(FILE * in, full_riff_tree_t * full) {
	int err = 0;

	while (1) {
		int c;
		if ((c = fgetc(in)) == EOF) break; ungetc(c, in);
		full->tree = realloc(full->tree, sizeof(riff_tree_t) * ++full->amount);
		if ((err = mk_riff_tree(in, &full->tree[full->amount - 1]))) goto err_out;
	}
err_out:
	return err;
}

void uninit_riff(full_riff_tree_t * full) {
	int i;
	if (!full) return;
	for (i = 0; i < full->amount; i++) free_riff_tree(&full->tree[i]);
	free(full->tree);
	free(full);
}

#ifdef RIFF_PROG
void print_riff_tree(riff_tree_t * tree, int indent) {
	char ind[indent + 1];
	int i;
	memset(ind, ' ', indent);
	ind[indent] = 0;

	if (tree->type == 0) {
		printf("%s%4.4s: offset: %d name: `%4.4s', len: %u (amount: %d)\n",
			ind, tree->name, tree->offset, tree->listname, tree->len, tree->amount);
		for (i = 0; i < tree->amount; i++) {
			print_riff_tree(&tree->tree[i], indent + 4);
		}
	} else {
		printf("%sDATA: offset: %d name: `%4.4s', len: %u\n",
			ind, tree->offset, tree->name, tree->len);
	}
}

int main(int argc, char * argv []) {
	FILE * in;
	full_riff_tree_t * full = init_riff();
	int err = 0;
	int i;
	if (argc < 2) { printf("bleh, more params you fool...\n"); return 1; }
	in = fopen(argv[1], "r");

	if ((err = get_full_riff_tree(in, full))) goto err_out;
	for (i = 0; i < full->amount; i++) {
		print_riff_tree(&full->tree[i], 0);
	}

err_out:
	uninit_riff(full);
	fclose(in);
	return err;
}
#endif
