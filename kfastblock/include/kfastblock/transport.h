#ifndef KFASTBLOCK_TRANSPORT_H
#define KFASTBLOCK_TRANSPORT_H

#include "kfastblock/control.h"
#include "kfastblock/meta.h"
#include "kfastblock/request.h"

int kfastblock_transport_init(void);
void kfastblock_transport_exit(void);
int kfastblock_transport_fetch_cluster_view(struct kfastblock_cluster_view *view,
				    const struct kfastblock_attach_spec *spec);
int kfastblock_transport_submit(struct kfastblock_request *kf_req);

#endif
