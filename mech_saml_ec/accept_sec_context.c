/*
 * Copyright (c) 2011, JANET(UK)
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of JANET(UK) nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * Establish a security context on the acceptor (server). These functions
 * wrap around libradsec and (thus) talk to a RADIUS server or proxy.
 */

#include "gssapiP_eap.h"

char* getSAMLRequest2(void);
int verifySAMLResponse(const char*,int,char*);

#if MECH_EAP
/*
 * Mark an acceptor context as ready for cryptographic operations
 */
static OM_uint32
acceptReadyEap(OM_uint32 *minor, gss_ctx_id_t ctx, gss_cred_id_t cred)
{
    OM_uint32 major, tmpMinor;
    VALUE_PAIR *vp;
    gss_buffer_desc nameBuf = GSS_C_EMPTY_BUFFER;

    gssEapReleaseName(&tmpMinor, &ctx->initiatorName);

    major = gssEapRadiusGetRawAvp(minor, ctx->acceptorCtx.vps,
                                  PW_USER_NAME, 0, &vp);
    if (major == GSS_S_COMPLETE && vp->length) {
        nameBuf.length = vp->length;
        nameBuf.value = vp->vp_strvalue;
    } else {
        ctx->gssFlags |= GSS_C_ANON_FLAG;
    }

    major = gssEapImportName(minor, &nameBuf,
                             (ctx->gssFlags & GSS_C_ANON_FLAG) ?
                                GSS_C_NT_ANONYMOUS : GSS_C_NT_USER_NAME,
                             ctx->mechanismUsed,
                             &ctx->initiatorName);
    if (GSS_ERROR(major))
        return major;

    major = gssEapRadiusGetRawAvp(minor, ctx->acceptorCtx.vps,
                                  PW_MS_MPPE_SEND_KEY, VENDORPEC_MS, &vp);
    if (GSS_ERROR(major)) {
        *minor = GSSEAP_KEY_UNAVAILABLE;
        return GSS_S_UNAVAILABLE;
    }

    major = sequenceInit(minor,
                         &ctx->seqState, ctx->recvSeq,
                         ((ctx->gssFlags & GSS_C_REPLAY_FLAG) != 0),
                         ((ctx->gssFlags & GSS_C_SEQUENCE_FLAG) != 0),
                         TRUE);
    if (GSS_ERROR(major))
        return major;

    major = gssEapCreateAttrContext(minor, cred, ctx,
                                    &ctx->initiatorName->attrCtx,
                                    &ctx->expiryTime);
    if (GSS_ERROR(major))
        return major;

    if (ctx->expiryTime != 0 && ctx->expiryTime < time(NULL)) {
        *minor = GSSEAP_CRED_EXPIRED;
        return GSS_S_CREDENTIALS_EXPIRED;
    }

    *minor = 0;
    return GSS_S_COMPLETE;
}
#endif

static OM_uint32
eapGssSmAcceptAcceptorName(OM_uint32 *minor,
                           gss_cred_id_t cred GSSEAP_UNUSED,
                           gss_ctx_id_t ctx,
                           gss_name_t target GSSEAP_UNUSED,
                           gss_OID mech GSSEAP_UNUSED,
                           OM_uint32 reqFlags GSSEAP_UNUSED,
                           OM_uint32 timeReq GSSEAP_UNUSED,
                           gss_channel_bindings_t chanBindings GSSEAP_UNUSED,
                           gss_buffer_t inputToken GSSEAP_UNUSED,
                           gss_buffer_t outputToken,
                           OM_uint32 *smFlags GSSEAP_UNUSED)
{
    OM_uint32 major;

    /* XXX TODO import and validate name from inputToken */

    if (ctx->acceptorName != GSS_C_NO_NAME) {
        /* Send desired target name to acceptor */
        major = gssEapDisplayName(minor, ctx->acceptorName,
                                  outputToken, NULL);
        if (GSS_ERROR(major))
            return major;
    }

    return GSS_S_CONTINUE_NEEDED;
}

#ifdef GSSEAP_DEBUG
static OM_uint32
eapGssSmAcceptVendorInfo(OM_uint32 *minor,
                         gss_cred_id_t cred GSSEAP_UNUSED,
                         gss_ctx_id_t ctx GSSEAP_UNUSED,
                         gss_name_t target GSSEAP_UNUSED,
                         gss_OID mech GSSEAP_UNUSED,
                         OM_uint32 reqFlags GSSEAP_UNUSED,
                         OM_uint32 timeReq GSSEAP_UNUSED,
                         gss_channel_bindings_t chanBindings GSSEAP_UNUSED,
                         gss_buffer_t inputToken,
                         gss_buffer_t outputToken GSSEAP_UNUSED,
                         OM_uint32 *smFlags GSSEAP_UNUSED)
{
    fprintf(stderr, "GSS-EAP: vendor: %.*s\n",
            (int)inputToken->length, (char *)inputToken->value);

    *minor = 0;
    return GSS_S_CONTINUE_NEEDED;
}
#endif

static void random_hex(char* s, size_t len) {
    static const char hexdigit[] = "0123456789abcdef";
    size_t i;
    for ( i = 0; i < len - 1; ++i )
        s[i] = hexdigit[random() % (sizeof(hexdigit) - 1)];
    s[len - 1] = 0;
}


/*
 * Emit a identity EAP request to force the initiator (peer) to identify
 * itself.
 */
static OM_uint32
eapGssSmAcceptIdentity(OM_uint32 *minor,
                       gss_cred_id_t cred,
                       gss_ctx_id_t ctx,
                       gss_name_t target GSSEAP_UNUSED,
                       gss_OID mech GSSEAP_UNUSED,
                       OM_uint32 reqFlags GSSEAP_UNUSED,
                       OM_uint32 timeReq GSSEAP_UNUSED,
                       gss_channel_bindings_t chanBindings GSSEAP_UNUSED,
                       gss_buffer_t inputToken,
                       gss_buffer_t outputToken,
                       OM_uint32 *smFlags)
{
    OM_uint32 major;
    struct wpabuf *reqData;
    gss_buffer_desc pktBuffer;
    char *saml_req;

    if (!gssEapCredAvailable(cred, ctx->mechanismUsed)) {
        *minor = GSSEAP_CRED_MECH_MISMATCH;
        return GSS_S_BAD_MECH;
    }

    if (inputToken != GSS_C_NO_BUFFER && inputToken->length != 0) {
        *minor = GSSEAP_WRONG_SIZE;
        return GSS_S_DEFECTIVE_TOKEN;
    }

#ifdef MECH_EAP
    reqData = eap_msg_alloc(EAP_VENDOR_IETF, EAP_TYPE_IDENTITY, 0,
                            EAP_CODE_REQUEST, 0);
    if (reqData == NULL) {
        *minor = ENOMEM;
        return GSS_S_FAILURE;
    }

    pktBuffer.length = wpabuf_len(reqData);
    pktBuffer.value = (void *)wpabuf_head(reqData);

    major = duplicateBuffer(minor, &pktBuffer, outputToken);
    if (GSS_ERROR(major))
        return major;

    wpabuf_free(reqData);
#else
    saml_req = getSAMLRequest2();
    major = makeStringBuffer(minor, saml_req?:"", outputToken);
    fprintf(stdout, "--- SENDING SAML_AUTHREQUEST: ---\n%s\n", 
           (char *)outputToken->value);
    free(saml_req);
    saml_req = NULL;
#endif

    GSSEAP_SM_TRANSITION_NEXT(ctx);

    *minor = 0;
    *smFlags |= SM_FLAG_OUTPUT_TOKEN_CRITICAL;

    return GSS_S_CONTINUE_NEEDED;
}

#ifdef MECH_EAP
/*
 * Returns TRUE if the input token contains an EAP identity response.
 */
static int
isIdentityResponseP(gss_buffer_t inputToken)
{
    struct wpabuf respData;

    wpabuf_set(&respData, inputToken->value, inputToken->length);

    return (eap_get_type(&respData) == EAP_TYPE_IDENTITY);
}

/*
 * Save the asserted initiator identity from the EAP identity response.
 */
static OM_uint32
importInitiatorIdentity(OM_uint32 *minor,
                        gss_ctx_id_t ctx,
                        gss_buffer_t inputToken)
{
    OM_uint32 tmpMinor;
    struct wpabuf respData;
    const unsigned char *pos;
    size_t len;
    gss_buffer_desc nameBuf;

    wpabuf_set(&respData, inputToken->value, inputToken->length);

    pos = eap_hdr_validate(EAP_VENDOR_IETF, EAP_TYPE_IDENTITY,
                           &respData, &len);
    if (pos == NULL) {
        *minor = GSSEAP_PEER_BAD_MESSAGE;
        return GSS_S_DEFECTIVE_TOKEN;
    }

    nameBuf.value = (void *)pos;
    nameBuf.length = len;

    gssEapReleaseName(&tmpMinor, &ctx->initiatorName);

    return gssEapImportName(minor, &nameBuf, GSS_C_NT_USER_NAME,
                            ctx->mechanismUsed, &ctx->initiatorName);
}

/*
 * Pass the asserted initiator identity to the authentication server.
 */
static OM_uint32
setInitiatorIdentity(OM_uint32 *minor,
                     gss_ctx_id_t ctx,
                     VALUE_PAIR **vps)
{
    OM_uint32 major, tmpMinor;
    gss_buffer_desc nameBuf;

    /*
     * We should have got an EAP identity response, but if we didn't, then
     * we will just avoid sending User-Name. Note that radsecproxy requires
     * User-Name to be sent on every request (presumably so it can remain
     * stateless).
     */
    if (ctx->initiatorName != GSS_C_NO_NAME) {
        major = gssEapDisplayName(minor, ctx->initiatorName, &nameBuf, NULL);
        if (GSS_ERROR(major))
            return major;

        major = gssEapRadiusAddAvp(minor, vps, PW_USER_NAME, 0, &nameBuf);
        if (GSS_ERROR(major))
            return major;

        gss_release_buffer(&tmpMinor, &nameBuf);
    }

    *minor = 0;
    return GSS_S_COMPLETE;
}

/*
 * Pass the asserted acceptor identity to the authentication server.
 */
static OM_uint32
setAcceptorIdentity(OM_uint32 *minor,
                    gss_ctx_id_t ctx,
                    VALUE_PAIR **vps)
{
    OM_uint32 major;
    gss_buffer_desc nameBuf;
    krb5_context krbContext = NULL;
    krb5_principal krbPrinc;
    struct rs_context *rc = ctx->acceptorCtx.radContext;

    GSSEAP_ASSERT(rc != NULL);

    if (ctx->acceptorName == GSS_C_NO_NAME) {
        *minor = 0;
        return GSS_S_COMPLETE;
    }

    if ((ctx->acceptorName->flags & NAME_FLAG_SERVICE) == 0) {
        *minor = GSSEAP_BAD_SERVICE_NAME;
        return GSS_S_BAD_NAME;
    }

    krbPrinc = ctx->acceptorName->krbPrincipal;
    GSSEAP_ASSERT(krbPrinc != NULL);
    GSSEAP_ASSERT(KRB_PRINC_LENGTH(krbPrinc) >= 2);

    /* Acceptor-Service-Name */
    krbPrincComponentToGssBuffer(krbPrinc, 0, &nameBuf);

    major = gssEapRadiusAddAvp(minor, vps,
                               PW_GSS_ACCEPTOR_SERVICE_NAME,
                               VENDORPEC_UKERNA,
                               &nameBuf);
    if (GSS_ERROR(major))
        return major;

    /* Acceptor-Host-Name */
    krbPrincComponentToGssBuffer(krbPrinc, 1, &nameBuf);

    major = gssEapRadiusAddAvp(minor, vps,
                               PW_GSS_ACCEPTOR_HOST_NAME,
                               VENDORPEC_UKERNA,
                               &nameBuf);
    if (GSS_ERROR(major))
        return major;

    if (KRB_PRINC_LENGTH(krbPrinc) > 2) {
        /* Acceptor-Service-Specific */
        krb5_principal_data ssiPrinc = *krbPrinc;
        char *ssi;

        KRB_PRINC_LENGTH(&ssiPrinc) -= 2;
        KRB_PRINC_NAME(&ssiPrinc) += 2;

        *minor = krb5_unparse_name_flags(krbContext, &ssiPrinc,
                                         KRB5_PRINCIPAL_UNPARSE_NO_REALM, &ssi);
        if (*minor != 0)
            return GSS_S_FAILURE;

        nameBuf.value = ssi;
        nameBuf.length = strlen(ssi);

        major = gssEapRadiusAddAvp(minor, vps,
                                   PW_GSS_ACCEPTOR_SERVICE_SPECIFIC,
                                   VENDORPEC_UKERNA,
                                   &nameBuf);

        if (GSS_ERROR(major)) {
            krb5_free_unparsed_name(krbContext, ssi);
            return major;
        }
        krb5_free_unparsed_name(krbContext, ssi);
    }

    krbPrincRealmToGssBuffer(krbPrinc, &nameBuf);
    if (nameBuf.length != 0) {
        /* Acceptor-Realm-Name */
        major = gssEapRadiusAddAvp(minor, vps,
                                   PW_GSS_ACCEPTOR_REALM_NAME,
                                   VENDORPEC_UKERNA,
                                   &nameBuf);
        if (GSS_ERROR(major))
            return major;
    }

    *minor = 0;
    return GSS_S_COMPLETE;
}

/*
 * Allocate a RadSec handle
 */
static OM_uint32
createRadiusHandle(OM_uint32 *minor,
                   gss_cred_id_t cred,
                   gss_ctx_id_t ctx)
{
    struct gss_eap_acceptor_ctx *actx = &ctx->acceptorCtx;
    struct rs_error *err;
    const char *configStanza = "gss-eap";
    OM_uint32 major;

    GSSEAP_ASSERT(actx->radContext == NULL);
    GSSEAP_ASSERT(actx->radConn == NULL);
    GSSEAP_ASSERT(cred != GSS_C_NO_CREDENTIAL);

    major = gssEapCreateRadiusContext(minor, cred, &actx->radContext);
    if (GSS_ERROR(major))
        return major;

    if (cred->radiusConfigStanza.value != NULL)
        configStanza = (const char *)cred->radiusConfigStanza.value;

    if (rs_conn_create(actx->radContext, &actx->radConn, configStanza) != 0) {
        err = rs_err_conn_pop(actx->radConn);
        return gssEapRadiusMapError(minor, err);
    }

    if (actx->radServer != NULL) {
        if (rs_conn_select_peer(actx->radConn, actx->radServer) != 0) {
            err = rs_err_conn_pop(actx->radConn);
            return gssEapRadiusMapError(minor, err);
        }
    }

    *minor = 0;
    return GSS_S_COMPLETE;
}
#endif

/*
 * Process a EAP response from the initiator.
 */
static OM_uint32
eapGssSmAcceptAuthenticate(OM_uint32 *minor,
                           gss_cred_id_t cred,
                           gss_ctx_id_t ctx,
                           gss_name_t target GSSEAP_UNUSED,
                           gss_OID mech GSSEAP_UNUSED,
                           OM_uint32 reqFlags GSSEAP_UNUSED,
                           OM_uint32 timeReq GSSEAP_UNUSED,
                           gss_channel_bindings_t chanBindings GSSEAP_UNUSED,
                           gss_buffer_t inputToken,
                           gss_buffer_t outputToken,
                           OM_uint32 *smFlags)
{
    OM_uint32 major, tmpMinor;
    struct rs_connection *rconn;
    struct rs_request *request = NULL;
    struct rs_packet *req = NULL, *resp = NULL;
    struct radius_packet *frreq, *frresp;

#ifdef MECH_EAP
    if (ctx->acceptorCtx.radContext == NULL) {
        /* May be NULL from an imported partial context */
        major = createRadiusHandle(minor, cred, ctx);
        if (GSS_ERROR(major))
            goto cleanup;
    }

    if (isIdentityResponseP(inputToken)) {
        major = importInitiatorIdentity(minor, ctx, inputToken);
        if (GSS_ERROR(major))
            return major;
    }

    rconn = ctx->acceptorCtx.radConn;

    if (rs_packet_create_authn_request(rconn, &req, NULL, NULL) != 0) {
        major = gssEapRadiusMapError(minor, rs_err_conn_pop(rconn));
        goto cleanup;
    }
    frreq = rs_packet_frpkt(req);

    major = setInitiatorIdentity(minor, ctx, &frreq->vps);
    if (GSS_ERROR(major))
        goto cleanup;

    major = setAcceptorIdentity(minor, ctx, &frreq->vps);
    if (GSS_ERROR(major))
        goto cleanup;

    major = gssEapRadiusAddAvp(minor, &frreq->vps,
                               PW_EAP_MESSAGE, 0, inputToken);
    if (GSS_ERROR(major))
        goto cleanup;

    if (ctx->acceptorCtx.state.length != 0) {
        major = gssEapRadiusAddAvp(minor, &frreq->vps, PW_STATE, 0,
                                   &ctx->acceptorCtx.state);
        if (GSS_ERROR(major))
            goto cleanup;

        gss_release_buffer(&tmpMinor, &ctx->acceptorCtx.state);
    }

    if (rs_request_create(rconn, &request) != 0) {
        major = gssEapRadiusMapError(minor, rs_err_conn_pop(rconn));
        goto cleanup;
    }

    rs_request_add_reqpkt(request, req);
    req = NULL;

    if (rs_request_send(request, &resp) != 0) {
        major = gssEapRadiusMapError(minor, rs_err_conn_pop(rconn));
        goto cleanup;
    }

    GSSEAP_ASSERT(resp != NULL);

    frresp = rs_packet_frpkt(resp);
    switch (frresp->code) {
    case PW_ACCESS_CHALLENGE:
    case PW_AUTHENTICATION_ACK:
        break;
    case PW_AUTHENTICATION_REJECT:
        *minor = GSSEAP_RADIUS_AUTH_FAILURE;
        major = GSS_S_DEFECTIVE_CREDENTIAL;
        goto cleanup;
        break;
    default:
        *minor = GSSEAP_UNKNOWN_RADIUS_CODE;
        major = GSS_S_FAILURE;
        goto cleanup;
        break;
    }

    major = gssEapRadiusGetAvp(minor, frresp->vps, PW_EAP_MESSAGE, 0,
                               outputToken, TRUE);
    if (major == GSS_S_UNAVAILABLE && frresp->code == PW_ACCESS_CHALLENGE) {
        *minor = GSSEAP_MISSING_EAP_REQUEST;
        major = GSS_S_DEFECTIVE_TOKEN;
        goto cleanup;
    } else if (GSS_ERROR(major))
        goto cleanup;

    if (frresp->code == PW_ACCESS_CHALLENGE) {
        major = gssEapRadiusGetAvp(minor, frresp->vps, PW_STATE, 0,
                                   &ctx->acceptorCtx.state, TRUE);
        if (GSS_ERROR(major) && *minor != GSSEAP_NO_SUCH_ATTR)
            goto cleanup;
    } else {
        ctx->acceptorCtx.vps = frresp->vps;
        frresp->vps = NULL;

        major = acceptReadyEap(minor, ctx, cred);
        if (GSS_ERROR(major))
            goto cleanup;

        GSSEAP_SM_TRANSITION_NEXT(ctx);
    }

    major = GSS_S_CONTINUE_NEEDED;
    *minor = 0;
    *smFlags |= SM_FLAG_OUTPUT_TOKEN_CRITICAL;

cleanup:
    if (request != NULL)
        rs_request_destroy(request);
    if (req != NULL)
        rs_packet_destroy(req);
    if (resp != NULL)
        rs_packet_destroy(resp);
    if (GSSEAP_SM_STATE(ctx) == GSSEAP_STATE_INITIATOR_EXTS) {
        GSSEAP_ASSERT(major == GSS_S_CONTINUE_NEEDED);

        rs_conn_destroy(ctx->acceptorCtx.radConn);
        ctx->acceptorCtx.radConn = NULL;
    }

    return major;
#else
    return GSS_S_UNAVAILABLE;
#endif
}

static OM_uint32
eapGssSmAcceptGssFlags(OM_uint32 *minor,
                       gss_cred_id_t cred GSSEAP_UNUSED,
                       gss_ctx_id_t ctx,
                       gss_name_t target GSSEAP_UNUSED,
                       gss_OID mech GSSEAP_UNUSED,
                       OM_uint32 reqFlags GSSEAP_UNUSED,
                       OM_uint32 timeReq GSSEAP_UNUSED,
                       gss_channel_bindings_t chanBindings GSSEAP_UNUSED,
                       gss_buffer_t inputToken,
                       gss_buffer_t outputToken GSSEAP_UNUSED,
                       OM_uint32 *smFlags GSSEAP_UNUSED)
{
    unsigned char *p;
    OM_uint32 initiatorGssFlags;

    GSSEAP_ASSERT((ctx->flags & CTX_FLAG_KRB_REAUTH) == 0);

    if (inputToken->length < 4) {
        *minor = GSSEAP_TOK_TRUNC;
        return GSS_S_DEFECTIVE_TOKEN;
    }

    /* allow flags to grow for future expansion */
    p = (unsigned char *)inputToken->value + inputToken->length - 4;

    initiatorGssFlags = load_uint32_be(p);
    initiatorGssFlags &= GSSEAP_WIRE_FLAGS_MASK;

    ctx->gssFlags |= initiatorGssFlags;

    return GSS_S_CONTINUE_NEEDED;
}

static OM_uint32
eapGssSmAcceptGssChannelBindings(OM_uint32 *minor,
                                 gss_cred_id_t cred GSSEAP_UNUSED,
                                 gss_ctx_id_t ctx,
                                 gss_name_t target GSSEAP_UNUSED,
                                 gss_OID mech GSSEAP_UNUSED,
                                 OM_uint32 reqFlags GSSEAP_UNUSED,
                                 OM_uint32 timeReq GSSEAP_UNUSED,
                                 gss_channel_bindings_t chanBindings,
                                 gss_buffer_t inputToken,
                                 gss_buffer_t outputToken GSSEAP_UNUSED,
                                 OM_uint32 *smFlags GSSEAP_UNUSED)
{
    OM_uint32 major;
    gss_iov_buffer_desc iov[2];

    iov[0].type = GSS_IOV_BUFFER_TYPE_DATA | GSS_IOV_BUFFER_FLAG_ALLOCATE;
    iov[0].buffer.length = 0;
    iov[0].buffer.value = NULL;

    iov[1].type = GSS_IOV_BUFFER_TYPE_STREAM | GSS_IOV_BUFFER_FLAG_ALLOCATED;

    /* XXX necessary because decrypted in place and we verify it later */
    major = duplicateBuffer(minor, inputToken, &iov[1].buffer);
    if (GSS_ERROR(major))
        return major;

    major = gssEapUnwrapOrVerifyMIC(minor, ctx, NULL, NULL,
                                    iov, 2, TOK_TYPE_WRAP);
    if (GSS_ERROR(major)) {
        gssEapReleaseIov(iov, 2);
        return major;
    }

    if (chanBindings != GSS_C_NO_CHANNEL_BINDINGS &&
        !bufferEqual(&iov[0].buffer, &chanBindings->application_data)) {
        major = GSS_S_BAD_BINDINGS;
        *minor = GSSEAP_BINDINGS_MISMATCH;
    } else {
        major = GSS_S_CONTINUE_NEEDED;
        *minor = 0;
    }

    gssEapReleaseIov(iov, 2);

    return major;
}

static OM_uint32
eapGssSmAcceptInitiatorMIC(OM_uint32 *minor,
                           gss_cred_id_t cred GSSEAP_UNUSED,
                           gss_ctx_id_t ctx,
                           gss_name_t target GSSEAP_UNUSED,
                           gss_OID mech GSSEAP_UNUSED,
                           OM_uint32 reqFlags GSSEAP_UNUSED,
                           OM_uint32 timeReq GSSEAP_UNUSED,
                           gss_channel_bindings_t chanBindings GSSEAP_UNUSED,
                           gss_buffer_t inputToken,
                           gss_buffer_t outputToken GSSEAP_UNUSED,
                           OM_uint32 *smFlags GSSEAP_UNUSED)
{
    OM_uint32 major;

    major = gssEapVerifyTokenMIC(minor, ctx, inputToken);
    if (GSS_ERROR(major))
        return major;

    GSSEAP_SM_TRANSITION_NEXT(ctx);

    *minor = 0;
    return GSS_S_CONTINUE_NEEDED;
}

static OM_uint32
eapGssSmAcceptAcceptorMIC(OM_uint32 *minor,
                          gss_cred_id_t cred GSSEAP_UNUSED,
                          gss_ctx_id_t ctx,
                          gss_name_t target GSSEAP_UNUSED,
                          gss_OID mech GSSEAP_UNUSED,
                          OM_uint32 reqFlags GSSEAP_UNUSED,
                          OM_uint32 timeReq GSSEAP_UNUSED,
                          gss_channel_bindings_t chanBindings GSSEAP_UNUSED,
                          gss_buffer_t inputToken GSSEAP_UNUSED,
                          gss_buffer_t outputToken,
                          OM_uint32 *smFlags)
{
    OM_uint32 major;

    major = gssEapMakeTokenMIC(minor, ctx, outputToken);
    if (GSS_ERROR(major))
        return major;

    GSSEAP_SM_TRANSITION(ctx, GSSEAP_STATE_ESTABLISHED);

    *minor = 0;
    *smFlags |= SM_FLAG_OUTPUT_TOKEN_CRITICAL;

    return GSS_S_COMPLETE;
}

static struct gss_eap_sm eapGssAcceptorSm[] = {
#ifdef MECH_EAP
    {
        ITOK_TYPE_ACCEPTOR_NAME_REQ,
        ITOK_TYPE_ACCEPTOR_NAME_RESP,
        GSSEAP_STATE_INITIAL,
        0,
        eapGssSmAcceptAcceptorName
    },
#endif
#ifdef GSSEAP_DEBUG
    {
        ITOK_TYPE_VENDOR_INFO,
        ITOK_TYPE_NONE,
        GSSEAP_STATE_INITIAL,
        0,
        eapGssSmAcceptVendorInfo,
    },
#endif
    {
#if 1 /* def MECH_EAP */
        ITOK_TYPE_NONE,
#else
        ITOK_TYPE_EAP_REQ,
#endif
        ITOK_TYPE_EAP_REQ,
        GSSEAP_STATE_INITIAL,
        SM_ITOK_FLAG_REQUIRED,
        eapGssSmAcceptIdentity,
    },
    {
        ITOK_TYPE_EAP_RESP,
        ITOK_TYPE_EAP_REQ,
        GSSEAP_STATE_AUTHENTICATE,
        SM_ITOK_FLAG_REQUIRED,
        eapGssSmAcceptAuthenticate
    },
#ifdef MECH_EAP
    {
        ITOK_TYPE_GSS_FLAGS,
        ITOK_TYPE_NONE,
        GSSEAP_STATE_INITIATOR_EXTS,
        0,
        eapGssSmAcceptGssFlags
    },
    {
        ITOK_TYPE_GSS_CHANNEL_BINDINGS,
        ITOK_TYPE_NONE,
        GSSEAP_STATE_INITIATOR_EXTS,
        SM_ITOK_FLAG_REQUIRED,
        eapGssSmAcceptGssChannelBindings,
    },
    {
        ITOK_TYPE_INITIATOR_MIC,
        ITOK_TYPE_NONE,
        GSSEAP_STATE_INITIATOR_EXTS,
        SM_ITOK_FLAG_REQUIRED,
        eapGssSmAcceptInitiatorMIC,
    },
    {
        ITOK_TYPE_NONE,
        ITOK_TYPE_ACCEPTOR_MIC,
        GSSEAP_STATE_ACCEPTOR_EXTS,
        0,
        eapGssSmAcceptAcceptorMIC
    },
#endif
};

OM_uint32
gssEapAcceptSecContext(OM_uint32 *minor,
                       gss_ctx_id_t ctx,
                       gss_cred_id_t cred,
                       gss_buffer_t input_token,
                       gss_channel_bindings_t input_chan_bindings,
                       gss_name_t *src_name,
                       gss_OID *mech_type,
                       gss_buffer_t output_token,
                       OM_uint32 *ret_flags,
                       OM_uint32 *time_rec,
                       gss_cred_id_t *delegated_cred_handle)
{
    OM_uint32 major, tmpMinor;
    char *saml_req = NULL;
#ifndef MECH_EAP
    int initialContextToken = (ctx->mechanismUsed == GSS_C_NO_OID);
#endif

    if (cred == GSS_C_NO_CREDENTIAL) {
        if (ctx->cred == GSS_C_NO_CREDENTIAL) {
            major = gssEapAcquireCred(minor,
                                      GSS_C_NO_NAME,
                                      GSS_C_INDEFINITE,
                                      GSS_C_NO_OID_SET,
                                      GSS_C_ACCEPT,
                                      &ctx->cred,
                                      NULL,
                                      NULL);
            if (GSS_ERROR(major))
                goto cleanup;
        }

        cred = ctx->cred;
    }

    /*
     * Previously we acquired the credential mutex here, but it should not be
     * necessary as the acceptor does not access any mutable elements of the
     * credential handle.
     */

    /*
     * Calling gssEapInquireCred() forces the default acceptor credential name
     * to be resolved.
     */
    major = gssEapInquireCred(minor, cred, &ctx->acceptorName, NULL, NULL, NULL);
    if (GSS_ERROR(major))
        goto cleanup;

#ifdef MECH_EAP
    major = gssEapSmStep(minor,
                         cred,
                         ctx,
                         GSS_C_NO_NAME,
                         GSS_C_NO_OID,
                         0,
                         GSS_C_INDEFINITE,
                         input_chan_bindings,
                         input_token,
                         output_token,
                         eapGssAcceptorSm,
                         sizeof(eapGssAcceptorSm) / sizeof(eapGssAcceptorSm[0]));
#else
    if (initialContextToken) {
        gss_buffer_desc innerToken = GSS_C_EMPTY_BUFFER;

        // This sets ctx->mechanismUsed
        major = gssEapVerifyToken(minor, ctx, input_token, NULL,
                                  &innerToken);
        if (!GSS_ERROR(major)) {
            GSSEAP_ASSERT(oidEqual(ctx->mechanismUsed, GSS_SAMLEC_MECHANISM));
            /* innerToken must be empty */
            GSSEAP_ASSERT(innerToken.length == 0);

            saml_req = getSAMLRequest2();
            if (saml_req != NULL) {
                major = makeStringBuffer(minor, saml_req?:"", output_token);
                free(saml_req); saml_req = NULL;
            } else
                major = GSS_S_FAILURE;

            if (!GSS_ERROR(major)) {
                fprintf(stdout, "--- SENDING SAML_AUTHREQUEST: ---\n%s\n", 
                   (char *)output_token->value);
                major = GSS_S_CONTINUE_NEEDED;
            }
        }
    } else {

        // Allocate space for username string. MUST FREE LATER!!!
        // TODO: have verifySAMLResponse allocate for username
        char* username = calloc(128,sizeof(char));
        int result = verifySAMLResponse((char*)input_token->value,
                                        (int)input_token->length,
                                        username);

        if (result) {
            gss_buffer_desc buf = {0, NULL};
            fprintf(stdout,"Username = '%s'\n",username);
            major = makeStringBuffer(minor, username, &buf);
            if (major == GSS_S_COMPLETE)
                major = gss_import_name(minor, &buf, GSS_C_NT_USER_NAME,
                                 &ctx->initiatorName);
            major = GSS_S_COMPLETE;
        } else {
            major = GSS_S_FAILURE;
            *minor = GSSEAP_PEER_AUTH_FAILURE;
        }

        free(username); username = NULL;
    }
#endif
    if (GSS_ERROR(major))
        goto cleanup;

    if (mech_type != NULL) {
        OM_uint32 tmpMajor;

        tmpMajor = gssEapCanonicalizeOid(&tmpMinor, ctx->mechanismUsed, 0, mech_type);
        if (GSS_ERROR(tmpMajor)) {
            major = tmpMajor;
            *minor = tmpMinor;
            goto cleanup;
        }
    }
    if (ret_flags != NULL)
        *ret_flags = ctx->gssFlags;
    if (delegated_cred_handle != NULL)
        *delegated_cred_handle = GSS_C_NO_CREDENTIAL;

    if (major == GSS_S_COMPLETE) {
        if (src_name != NULL && ctx->initiatorName != GSS_C_NO_NAME) {
            major = gssEapDuplicateName(&tmpMinor, ctx->initiatorName, src_name);
            if (GSS_ERROR(major))
                goto cleanup;
        }
        if (time_rec != NULL) {
            major = gssEapContextTime(&tmpMinor, ctx, time_rec);
            if (GSS_ERROR(major))
                goto cleanup;
        }
    }

#ifdef MECH_EAP
    /* TODO VSY: Do we need to care about maintaining ctx->state ? */
    GSSEAP_ASSERT(CTX_IS_ESTABLISHED(ctx) || major == GSS_S_CONTINUE_NEEDED);
#endif

cleanup:
    return major;
}

OM_uint32 GSSAPI_CALLCONV
gss_accept_sec_context(OM_uint32 *minor,
                       gss_ctx_id_t *context_handle,
                       gss_cred_id_t cred,
                       gss_buffer_t input_token,
                       gss_channel_bindings_t input_chan_bindings,
                       gss_name_t *src_name,
                       gss_OID *mech_type,
                       gss_buffer_t output_token,
                       OM_uint32 *ret_flags,
                       OM_uint32 *time_rec,
                       gss_cred_id_t *delegated_cred_handle)
{
    OM_uint32 major, tmpMinor;
    gss_ctx_id_t ctx = *context_handle;

    *minor = 0;

    output_token->length = 0;
    output_token->value = NULL;

    if (src_name != NULL)
        *src_name = GSS_C_NO_NAME;

    if (input_token == GSS_C_NO_BUFFER || input_token->length == 0) {
        *minor = GSSEAP_TOK_TRUNC;
        return GSS_S_DEFECTIVE_TOKEN;
    }

    if (ctx == GSS_C_NO_CONTEXT) {
        major = gssEapAllocContext(minor, &ctx);
        if (GSS_ERROR(major))
            return major;

        *context_handle = ctx;
    }

    GSSEAP_MUTEX_LOCK(&ctx->mutex);

    major = gssEapAcceptSecContext(minor,
                                   ctx,
                                   cred,
                                   input_token,
                                   input_chan_bindings,
                                   src_name,
                                   mech_type,
                                   output_token,
                                   ret_flags,
                                   time_rec,
                                   delegated_cred_handle);

    GSSEAP_MUTEX_UNLOCK(&ctx->mutex);

    if (GSS_ERROR(major))
        gssEapReleaseContext(&tmpMinor, context_handle);

    return major;
}
