/*
 * libiio - Library for interfacing industrial I/O (IIO) devices
 *
 * Copyright (C) 2014 Analog Devices, Inc.
 * Author: Paul Cercueil <paul.cercueil@analog.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * */

#include "debug.h"
#include "iio-private.h"

#include <errno.h>
#include <libxml/tree.h>
#include <string.h>

static int add_attr_to_channel(struct iio_channel *chn, xmlNode *n)
{
	xmlAttr *attr;
	char **attrs, *name = NULL;

	for (attr = n->properties; attr; attr = attr->next) {
		if (!strcmp((char *) attr->name, "name")) {
			name = strdup((char *) attr->children->content);
		} else {
			WARNING("Unknown field \'%s\' in channel %s\n",
					attr->name, chn->id);
		}
	}

	if (!name) {
		ERROR("Incomplete attribute in channel %s\n", chn->id);
		goto err_free;
	}

	attrs = realloc(chn->attrs, (1 + chn->nb_attrs) * sizeof(char *));
	if (!attrs)
		goto err_free;

	attrs[chn->nb_attrs++] = name;
	chn->attrs = attrs;
	return 0;

err_free:
	if (name)
		free(name);
	return -1;
}

static int add_attr_to_device(struct iio_device *dev, xmlNode *n)
{
	xmlAttr *attr;
	char **attrs, *name = NULL;

	for (attr = n->properties; attr; attr = attr->next) {
		if (!strcmp((char *) attr->name, "name")) {
			name = strdup((char *) attr->children->content);
		} else {
			WARNING("Unknown field \'%s\' in device %s\n",
					attr->name, dev->id);
		}
	}

	if (!name) {
		ERROR("Incomplete attribute in device %s\n", dev->id);
		goto err_free;
	}

	attrs = realloc(dev->attrs, (1 + dev->nb_attrs) * sizeof(char *));
	if (!attrs)
		goto err_free;

	attrs[dev->nb_attrs++] = name;
	dev->attrs = attrs;
	return 0;

err_free:
	if (name)
		free(name);
	return -1;
}

static struct iio_channel * create_channel(struct iio_device *dev, xmlNode *n)
{
	xmlAttr *attr;
	struct iio_channel *chn = calloc(1, sizeof(*chn));
	if (!chn)
		return NULL;

	chn->dev = dev;

	for (attr = n->properties; attr; attr = attr->next) {
		const char *name = (const char *) attr->name,
		      *content = (const char *) attr->children->content;
		if (!strcmp(name, "name")) {
			chn->name = strdup(content);
		} else if (!strcmp(name, "id")) {
			chn->id = strdup(content);
		} else if (!strcmp(name, "type")) {
			if (!strcmp(content, "output"))
				chn->is_output = true;
			else if (strcmp(content, "input"))
				WARNING("Unknown channel type %s\n", content);
		} else {
			WARNING("Unknown attribute \'%s\' in <channel>\n",
					name);
		}
	}

	if (!chn->id) {
		ERROR("Incomplete <attribute>\n");
		goto err_free_channel;
	}

	for (n = n->children; n; n = n->next) {
		if (!strcmp((char *) n->name, "attribute")) {
			if (add_attr_to_channel(chn, n) < 0)
				goto err_free_channel;
		} else if (strcmp((char *) n->name, "text")) {
			WARNING("Unknown children \'%s\' in <device>\n",
					n->name);
			continue;
		}
	}

	return chn;

err_free_channel:
	free_channel(chn);
	return NULL;
}

static struct iio_device * create_device(struct iio_context *ctx, xmlNode *n)
{
	xmlAttr *attr;
	struct iio_device *dev = calloc(1, sizeof(*dev));
	if (!dev)
		return NULL;

	dev->ctx = ctx;

	for (attr = n->properties; attr; attr = attr->next) {
		if (!strcmp((char *) attr->name, "name")) {
			dev->name = strdup((char *) attr->children->content);
		} else if (!strcmp((char *) attr->name, "id")) {
			dev->id = strdup((char *) attr->children->content);
		} else {
			WARNING("Unknown attribute \'%s\' in <context>\n",
					attr->name);
		}
	}

	if (!dev->id) {
		ERROR("Unable to read device ID\n");
		goto err_free_device;
	}

	for (n = n->children; n; n = n->next) {
		if (!strcmp((char *) n->name, "channel")) {
			struct iio_channel **chns,
					   *chn = create_channel(dev, n);
			if (!chn) {
				ERROR("Unable to create channel\n");
				goto err_free_device;
			}

			chns = realloc(dev->channels, (1 + dev->nb_channels) *
					sizeof(struct iio_channel *));
			if (!chns) {
				ERROR("Unable to allocate memory\n");
				free(chn);
				goto err_free_device;
			}

			chns[dev->nb_channels++] = chn;
			dev->channels = chns;
		} else if (!strcmp((char *) n->name, "attribute")) {
			if (add_attr_to_device(dev, n) < 0)
				goto err_free_device;
		} else if (strcmp((char *) n->name, "text")) {
			WARNING("Unknown children \'%s\' in <device>\n",
					n->name);
			continue;
		}
	}

	return dev;

err_free_device:
	free_device(dev);
	return NULL;
}

static struct iio_backend_ops xml_ops = {
};

static struct iio_context * iio_create_xml_context_helper(xmlDoc *doc)
{
	unsigned int i;
	xmlNode *root, *n;
	struct iio_context *ctx = calloc(1, sizeof(*ctx));
	if (!ctx)
		return NULL;

	ctx->name = "xml";
	ctx->ops = &xml_ops;

	root = xmlDocGetRootElement(doc);
	if (strcmp((char *) root->name, "context")) {
		ERROR("Unrecognized XML file\n");
		goto err_free_ctx;
	}

	for (n = root->children; n; n = n->next) {
		struct iio_device **devs, *dev;

		if (strcmp((char *) n->name, "device")) {
			if (strcmp((char *) n->name, "text"))
				WARNING("Unknown children \'%s\' in "
						"<context>\n", n->name);
			continue;
		}

		dev = create_device(ctx, n);
		if (!dev) {
			ERROR("Unable to create device\n");
			goto err_free_devices;
		}

		devs = realloc(ctx->devices, (1 + ctx->nb_devices) *
				sizeof(struct iio_device *));
		if (!devs) {
			ERROR("Unable to allocate memory\n");
			free(dev);
			goto err_free_devices;
		}

		devs[ctx->nb_devices++] = dev;
		ctx->devices = devs;
	}

	return ctx;

err_free_devices:
	for (i = 0; i < ctx->nb_devices; i++)
		free_device(ctx->devices[i]);
	if (ctx->nb_devices)
		free(ctx->devices);
err_free_ctx:
	free(ctx);
	return NULL;
}

struct iio_context * iio_create_xml_context(const char *xml_file)
{
	struct iio_context *ctx;
	xmlDoc *doc;

	LIBXML_TEST_VERSION;

	doc = xmlReadFile(xml_file, NULL, XML_PARSE_DTDVALID);
	if (!doc) {
		ERROR("Unable to parse XML file\n");
		return NULL;
	}

	ctx = iio_create_xml_context_helper(doc);
	xmlFreeDoc(doc);
	xmlCleanupParser();
	return ctx;
}

struct iio_context * iio_create_xml_context_mem(const char *xml, size_t len)
{
	struct iio_context *ctx;
	xmlDoc *doc;

	LIBXML_TEST_VERSION;

	doc = xmlReadMemory(xml, len, NULL, NULL, XML_PARSE_DTDVALID);
	if (!doc) {
		ERROR("Unable to parse XML file\n");
		return NULL;
	}

	ctx = iio_create_xml_context_helper(doc);
	xmlFreeDoc(doc);
	xmlCleanupParser();
	return ctx;
}