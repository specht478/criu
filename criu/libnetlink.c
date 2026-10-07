#include <linux/types.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <libnl3/netlink/attr.h>
#include <libnl3/netlink/msg.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <time.h>

#include "libnetlink.h"
#include "util.h"

static int nlmsg_receive(char *buf, int len, int (*cb)(struct nlmsghdr *, struct ns_id *ns, void *),
			 int (*err_cb)(int, struct ns_id *, void *), struct ns_id *ns, void *arg)
{
	struct nlmsghdr *hdr;

	for (hdr = (struct nlmsghdr *)buf; NLMSG_OK(hdr, len); hdr = NLMSG_NEXT(hdr, len)) {
		if (hdr->nlmsg_seq != CR_NLMSG_SEQ)
			continue;
		if (hdr->nlmsg_type == NLMSG_DONE) {
			int *length = (int *)NLMSG_DATA(hdr);
			if (*length < 0)
				return err_cb(*length, ns, arg);
			return 0;
		}
		if (hdr->nlmsg_type == NLMSG_ERROR) {
			struct nlmsgerr *err = (struct nlmsgerr *)NLMSG_DATA(hdr);

			if (hdr->nlmsg_len - sizeof(*hdr) < sizeof(struct nlmsgerr)) {
				pr_err("ERROR truncated\n");
				return -1;
			}

			if (err->error == 0)
				return 0;

			return err_cb(err->error, ns, arg);
		}
		if (cb(hdr, ns, arg))
			return -1;
	}

	return 1;
}

/*
 * Default error handler: just point our an error
 * and pass up to caller.
 */
static int rtnl_return_err(int err, struct ns_id *ns, void *arg)
{
	errno = -err;
	pr_perror("%d reported by netlink", err);
	return err;
}

static int rtnl_remaining_ms(const struct timespec *deadline)
{
	struct timespec now;
	long long remaining;

	if (clock_gettime(CLOCK_MONOTONIC, &now))
		return -errno;
	remaining = (deadline->tv_sec - now.tv_sec) * 1000000000LL + deadline->tv_nsec - now.tv_nsec;
	if (remaining <= 0)
		return -ETIMEDOUT;
	return (remaining + 999999) / 1000000;
}

static int rtnl_wait(int nl, short events, const struct timespec *deadline)
{
	struct pollfd descriptor = { .fd = nl, .events = events };
	int remaining, ready;

	if (!deadline)
		return 0;
	for (;;) {
		remaining = rtnl_remaining_ms(deadline);
		if (remaining < 0)
			return remaining;
		ready = poll(&descriptor, 1, remaining);
		if (ready > 0)
			return 0;
		if (!ready)
			return -ETIMEDOUT;
		if (errno != EINTR)
			return -errno;
	}
}

int do_rtnl_req_timeout(int nl, void *req, int size,
			int (*receive_callback)(struct nlmsghdr *h, struct ns_id *ns, void *),
			int (*error_callback)(int err, struct ns_id *ns, void *arg), struct ns_id *ns,
			void *arg, int timeout_ms)
{
	struct msghdr msg;
	struct sockaddr_nl nladdr;
	struct iovec iov;
	struct timespec expires, *deadline = NULL;
	static char buf[16384];
	int err, flags = 0;

	if (timeout_ms < 0)
		return -EINVAL;
	if (timeout_ms) {
		if (clock_gettime(CLOCK_MONOTONIC, &expires))
			return -errno;
		expires.tv_sec += timeout_ms / 1000;
		expires.tv_nsec += (timeout_ms % 1000) * 1000000L;
		if (expires.tv_nsec >= 1000000000L) {
			expires.tv_sec++;
			expires.tv_nsec -= 1000000000L;
		}
		deadline = &expires;
		flags = MSG_DONTWAIT;
	}

	if (!error_callback)
		error_callback = rtnl_return_err;

	memset(&msg, 0, sizeof(msg));
	msg.msg_name = &nladdr;
	msg.msg_namelen = sizeof(nladdr);
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;

	memset(&nladdr, 0, sizeof(nladdr));
	nladdr.nl_family = AF_NETLINK;

	iov.iov_base = req;
	iov.iov_len = size;

	for (;;) {
		err = rtnl_wait(nl, POLLOUT, deadline);
		if (err)
			goto err;
		if (sendmsg(nl, &msg, flags) >= 0)
			break;
		if (deadline && (errno == EINTR || errno == EAGAIN))
			continue;
		err = -errno;
		pr_perror("Can't send request message");
		goto err;
	}

	iov.iov_base = buf;
	iov.iov_len = sizeof(buf);

	while (1) {
		memset(&msg, 0, sizeof(msg));
		msg.msg_name = &nladdr;
		msg.msg_namelen = sizeof(nladdr);
		msg.msg_iov = &iov;
		msg.msg_iovlen = 1;

		err = rtnl_wait(nl, POLLIN, deadline);
		if (err)
			goto err;
		err = recvmsg(nl, &msg, flags);
		if (err < 0) {
			if (errno == EINTR || (deadline && errno == EAGAIN))
				continue;
			else {
				err = -errno;
				pr_perror("Error receiving nl report");
				goto err;
			}
		}
		if (err == 0)
			break;

		if (msg.msg_flags & MSG_TRUNC) {
			pr_err("Message truncated\n");
			err = -EMSGSIZE;
			goto err;
		}

		err = nlmsg_receive(buf, err, receive_callback, error_callback, ns, arg);
		if (err < 0)
			goto err;
		if (err == 0)
			break;
	}

	if (deadline) {
		err = rtnl_remaining_ms(deadline);
		if (err < 0)
			goto err;
	}
	return 0;

err:
	if (err == -ETIMEDOUT)
		pr_err("Netlink request timed out\n");
	return err;
}

int do_rtnl_req(int nl, void *req, int size, int (*receive_callback)(struct nlmsghdr *h, struct ns_id *ns, void *),
		int (*error_callback)(int err, struct ns_id *ns, void *arg), struct ns_id *ns, void *arg)
{
	return do_rtnl_req_timeout(nl, req, size, receive_callback, error_callback, ns, arg, 0);
}

int addattr_l(struct nlmsghdr *n, int maxlen, int type, const void *data, int alen)
{
	int len = nla_attr_size(alen);
	struct rtattr *rta;

	if (NLMSG_ALIGN(n->nlmsg_len) + RTA_ALIGN(len) > maxlen) {
		pr_err("addattr_l ERROR: message exceeded bound of %d\n", maxlen);
		return -1;
	}

	rta = NLMSG_TAIL(n);
	rta->rta_type = type;
	rta->rta_len = len;
	memcpy(RTA_DATA(rta), data, alen);
	n->nlmsg_len = NLMSG_ALIGN(n->nlmsg_len) + RTA_ALIGN(len);
	return 0;
}

/*
 * Here is a workaround for a bug in libnl-3:
 * 6a8d90f5fec4 "attr: Allow attribute type 0
 */

/**
 * Create attribute index based on a stream of attributes.
 * @arg tb		Index array to be filled (maxtype+1 elements).
 * @arg maxtype		Maximum attribute type expected and accepted.
 * @arg head		Head of attribute stream.
 * @arg len		Length of attribute stream.
 * @arg policy		Attribute validation policy.
 *
 * Iterates over the stream of attributes and stores a pointer to each
 * attribute in the index array using the attribute type as index to
 * the array. Attribute with a type greater than the maximum type
 * specified will be silently ignored in order to maintain backwards
 * compatibility. If \a policy is not NULL, the attribute will be
 * validated using the specified policy.
 *
 * @see nla_validate
 * @return 0 on success or a negative error code.
 */
int __wrap_nla_parse(struct nlattr *tb[], int maxtype, struct nlattr *head, int len, struct nla_policy *policy)
{
	struct nlattr *nla;
	int rem;

	memset(tb, 0, sizeof(struct nlattr *) * (maxtype + 1));

	nla_for_each_attr(nla, head, len, rem) {
		int type = nla_type(nla);

		if (type > maxtype)
			continue;

		if (tb[type])
			pr_warn("Attribute of type %#x found multiple times in message, "
				"previous attribute is being ignored.\n",
				type);

		tb[type] = nla;
	}

	if (rem > 0)
		pr_warn("netlink: %d bytes leftover after parsing "
			"attributes.\n",
			rem);

	return 0;
}

/**
 * parse attributes of a netlink message
 * @arg nlh		netlink message header
 * @arg hdrlen		length of family specific header
 * @arg tb		destination array with maxtype+1 elements
 * @arg maxtype		maximum attribute type to be expected
 * @arg policy		validation policy
 *
 * See nla_parse()
 */
int __wrap_nlmsg_parse(struct nlmsghdr *nlh, int hdrlen, struct nlattr *tb[], int maxtype, struct nla_policy *policy)
{
	if (!nlmsg_valid_hdr(nlh, hdrlen))
		return -NLE_MSG_TOOSHORT;

	return nla_parse(tb, maxtype, nlmsg_attrdata(nlh, hdrlen), nlmsg_attrlen(nlh, hdrlen), policy);
}
