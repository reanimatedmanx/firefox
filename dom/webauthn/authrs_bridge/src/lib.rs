/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#[macro_use]
extern crate log;

#[macro_use]
extern crate xpcom;

use authenticator::{
    authenticatorservice::{RegisterArgs, SignArgs},
    ctap2::attestation::AttestationObject,
    ctap2::commands::get_info::AuthenticatorVersion,
    ctap2::server::{
        AuthenticationExtensionsClientInputs, PublicKeyCredentialDescriptor,
        PublicKeyCredentialParameters, PublicKeyCredentialUserEntity, RelyingParty,
        ResidentKeyRequirement, UserVerificationRequirement,
    },
    errors::AuthenticatorError,
    statecallback::StateCallback,
    Pin, RegisterResult, SignResult, StateMachine, StatusPinUv, StatusUpdate,
};
use base64::Engine;
use cstr::cstr;
use moz_task::{get_main_thread, RunnableBuilder};
use nserror::{
    nsresult, NS_ERROR_DOM_INVALID_STATE_ERR, NS_ERROR_DOM_NOT_ALLOWED_ERR, NS_ERROR_FAILURE,
    NS_ERROR_INVALID_ARG, NS_ERROR_NOT_AVAILABLE, NS_ERROR_NOT_IMPLEMENTED, NS_ERROR_NULL_POINTER,
    NS_OK,
};
use nsstring::{nsACString, nsCString, nsString};
use serde::Serialize;
use serde_cbor;
use serde_json::json;
use std::cell::RefCell;
use std::fmt::Write;
use std::sync::mpsc::{channel, Receiver, RecvError, Sender};
use std::sync::{Arc, Mutex};
use thin_vec::{thin_vec, ThinVec};
use xpcom::interfaces::{
    nsICredentialParameters, nsICtapRegisterArgs, nsICtapRegisterResult, nsICtapSignArgs,
    nsICtapSignResult, nsIObserverService, nsIWebAuthnAttObj, nsIWebAuthnController,
    nsIWebAuthnTransport,
};
use xpcom::{xpcom_method, RefPtr};

mod test_token;
use test_token::TestTokenManager;

fn authrs_to_nserror(e: &AuthenticatorError) -> nsresult {
    match e {
        AuthenticatorError::CredentialExcluded => NS_ERROR_DOM_INVALID_STATE_ERR,
        _ => NS_ERROR_DOM_NOT_ALLOWED_ERR,
    }
}

fn error_cancels_prompts(e: &AuthenticatorError) -> bool {
    match e {
        AuthenticatorError::CredentialExcluded | AuthenticatorError::PinError(_) => false,
        _ => true,
    }
}

// Using serde(tag="type") makes it so that, for example, BrowserPromptType::Cancel is serialized
// as '{ type: "cancel" }', and BrowserPromptType::PinInvalid { retries: 5 } is serialized as
// '{type: "pin-invalid", retries: 5}'.
#[derive(Serialize)]
#[serde(tag = "type", rename_all = "kebab-case")]
enum BrowserPromptType<'a> {
    AlreadyRegistered,
    Cancel,
    DeviceBlocked,
    PinAuthBlocked,
    PinNotSet,
    Presence,
    SelectDevice,
    UvBlocked,
    PinRequired,
    PinInvalid {
        retries: Option<u8>,
    },
    RegisterDirect,
    UvInvalid {
        retries: Option<u8>,
    },
    SelectSignResult {
        entities: &'a [PublicKeyCredentialUserEntity],
    },
}

#[derive(Serialize)]
struct BrowserPromptMessage<'a> {
    prompt: BrowserPromptType<'a>,
    tid: u64,
    origin: Option<&'a str>,
    #[serde(rename = "browsingContextId")]
    browsing_context_id: Option<u64>,
}

fn send_prompt(
    prompt: BrowserPromptType,
    tid: u64,
    origin: Option<&str>,
    browsing_context_id: Option<u64>,
) -> Result<(), nsresult> {
    let main_thread = get_main_thread()?;
    let mut json = nsString::new();
    write!(
        json,
        "{}",
        json!(&BrowserPromptMessage {
            prompt,
            tid,
            origin,
            browsing_context_id
        })
    )
    .or(Err(NS_ERROR_FAILURE))?;
    RunnableBuilder::new("AuthrsTransport::send_prompt", move || {
        if let Ok(obs_svc) = xpcom::components::Observer::service::<nsIObserverService>() {
            unsafe {
                obs_svc.NotifyObservers(
                    std::ptr::null(),
                    cstr!("webauthn-prompt").as_ptr(),
                    json.as_ptr(),
                );
            }
        }
    })
    .dispatch(main_thread.coerce())
}

fn cancel_prompts(tid: u64) -> Result<(), nsresult> {
    send_prompt(BrowserPromptType::Cancel, tid, None, None)?;
    Ok(())
}

type RegisterResultOrError = Result<RegisterResult, AuthenticatorError>;

#[xpcom(implement(nsICtapRegisterResult), atomic)]
pub struct CtapRegisterResult {
    result: RegisterResultOrError,
}

impl CtapRegisterResult {
    xpcom_method!(get_attestation_object => GetAttestationObject() -> ThinVec<u8>);
    fn get_attestation_object(&self) -> Result<ThinVec<u8>, nsresult> {
        let mut out = ThinVec::new();
        let make_cred_res = self.result.as_ref().or(Err(NS_ERROR_FAILURE))?;
        serde_cbor::to_writer(&mut out, &make_cred_res.att_obj).or(Err(NS_ERROR_FAILURE))?;
        Ok(out)
    }

    xpcom_method!(get_credential_id => GetCredentialId() -> ThinVec<u8>);
    fn get_credential_id(&self) -> Result<ThinVec<u8>, nsresult> {
        let mut out = ThinVec::new();
        if let Ok(make_cred_res) = &self.result {
            if let Some(credential_data) = &make_cred_res.att_obj.auth_data.credential_data {
                out.extend_from_slice(&credential_data.credential_id);
                return Ok(out);
            }
        }
        Err(NS_ERROR_FAILURE)
    }

    xpcom_method!(get_transports => GetTransports() -> ThinVec<nsString>);
    fn get_transports(&self) -> Result<ThinVec<nsString>, nsresult> {
        if self.result.is_err() {
            return Err(NS_ERROR_FAILURE);
        }
        // The list that we return here might be included in a future GetAssertion request as a
        // hint as to which transports to try. We currently only support the USB transport. If
        // that changes, we will need a mechanism to track which transport was used for a
        // request.
        Ok(thin_vec![nsString::from("usb")])
    }

    xpcom_method!(get_cred_props_rk => GetCredPropsRk() -> bool);
    fn get_cred_props_rk(&self) -> Result<bool, nsresult> {
        let result = self.result.as_ref().or(Err(NS_ERROR_FAILURE))?;
        let cred_props = result
            .extensions
            .cred_props
            .as_ref()
            .ok_or(NS_ERROR_NOT_AVAILABLE)?;
        Ok(cred_props.rk)
    }

    xpcom_method!(get_status => GetStatus() -> nsresult);
    fn get_status(&self) -> Result<nsresult, nsresult> {
        match &self.result {
            Ok(_) => Ok(NS_OK),
            Err(e) => Ok(authrs_to_nserror(e)),
        }
    }
}

#[xpcom(implement(nsIWebAuthnAttObj), atomic)]
pub struct WebAuthnAttObj {
    att_obj: AttestationObject,
}

impl WebAuthnAttObj {
    xpcom_method!(get_attestation_object => GetAttestationObject() -> ThinVec<u8>);
    fn get_attestation_object(&self) -> Result<ThinVec<u8>, nsresult> {
        let mut out = ThinVec::new();
        serde_cbor::to_writer(&mut out, &self.att_obj).or(Err(NS_ERROR_FAILURE))?;
        Ok(out)
    }

    xpcom_method!(get_authenticator_data => GetAuthenticatorData() -> ThinVec<u8>);
    fn get_authenticator_data(&self) -> Result<ThinVec<u8>, nsresult> {
        // TODO(https://github.com/mozilla/authenticator-rs/issues/302) use to_writer
        Ok(self.att_obj.auth_data.to_vec().into())
    }

    xpcom_method!(get_public_key => GetPublicKey() -> ThinVec<u8>);
    fn get_public_key(&self) -> Result<ThinVec<u8>, nsresult> {
        let Some(credential_data) = &self.att_obj.auth_data.credential_data else {
            return Err(NS_ERROR_FAILURE);
        };
        Ok(credential_data
            .credential_public_key
            .der_spki()
            .or(Err(NS_ERROR_NOT_AVAILABLE))?
            .into())
    }

    xpcom_method!(get_public_key_algorithm => GetPublicKeyAlgorithm() -> i32);
    fn get_public_key_algorithm(&self) -> Result<i32, nsresult> {
        if let Some(credential_data) = &self.att_obj.auth_data.credential_data {
            // safe to cast to i32 by inspection of defined values
            Ok(credential_data.credential_public_key.alg as i32)
        } else {
            Err(NS_ERROR_FAILURE)
        }
    }
}

type SignResultOrError = Result<SignResult, AuthenticatorError>;

#[xpcom(implement(nsICtapSignResult), atomic)]
pub struct CtapSignResult {
    result: SignResultOrError,
}

impl CtapSignResult {
    xpcom_method!(get_credential_id => GetCredentialId() -> ThinVec<u8>);
    fn get_credential_id(&self) -> Result<ThinVec<u8>, nsresult> {
        let rv = NS_ERROR_FAILURE;
        let inner = self.result.as_ref().or(Err(rv))?;
        let cred = inner.assertion.credentials.as_ref().ok_or(rv)?;
        Ok(cred.id.as_slice().into())
    }

    xpcom_method!(get_signature => GetSignature() -> ThinVec<u8>);
    fn get_signature(&self) -> Result<ThinVec<u8>, nsresult> {
        let inner = self.result.as_ref().or(Err(NS_ERROR_FAILURE))?;
        Ok(inner.assertion.signature.as_slice().into())
    }

    xpcom_method!(get_authenticator_data => GetAuthenticatorData() -> ThinVec<u8>);
    fn get_authenticator_data(&self) -> Result<ThinVec<u8>, nsresult> {
        let inner = self.result.as_ref().or(Err(NS_ERROR_FAILURE))?;
        Ok(inner.assertion.auth_data.to_vec().into())
    }

    xpcom_method!(get_user_handle => GetUserHandle() -> ThinVec<u8>);
    fn get_user_handle(&self) -> Result<ThinVec<u8>, nsresult> {
        let rv = NS_ERROR_NOT_AVAILABLE;
        let inner = self.result.as_ref().or(Err(rv))?;
        let user = &inner.assertion.user.as_ref().ok_or(rv)?;
        Ok(user.id.as_slice().into())
    }

    xpcom_method!(get_user_name => GetUserName() -> nsACString);
    fn get_user_name(&self) -> Result<nsCString, nsresult> {
        let rv = NS_ERROR_NOT_AVAILABLE;
        let inner = self.result.as_ref().or(Err(rv))?;
        let user = inner.assertion.user.as_ref().ok_or(rv)?;
        let name = user.name.as_ref().ok_or(rv)?;
        Ok(nsCString::from(name))
    }

    xpcom_method!(get_used_app_id => GetUsedAppId() -> bool);
    fn get_used_app_id(&self) -> Result<bool, nsresult> {
        let inner = self.result.as_ref().or(Err(NS_ERROR_FAILURE))?;
        let app_id = inner.extensions.app_id.ok_or(NS_ERROR_NOT_AVAILABLE)?;
        Ok(app_id)
    }

    xpcom_method!(get_status => GetStatus() -> nsresult);
    fn get_status(&self) -> Result<nsresult, nsresult> {
        match &self.result {
            Ok(_) => Ok(NS_OK),
            Err(e) => Ok(authrs_to_nserror(e)),
        }
    }
}

/// Controller wraps a raw pointer to an nsIWebAuthnController. The AuthrsTransport struct holds a
/// Controller which we need to initialize from the SetController XPCOM method.  Since XPCOM
/// methods take &self, Controller must have interior mutability.
#[derive(Clone)]
struct Controller(RefCell<*const nsIWebAuthnController>);

/// Our implementation of nsIWebAuthnController in WebAuthnController.cpp has the property that its
/// XPCOM methods are safe to call from any thread, hence a raw pointer to an nsIWebAuthnController
/// is Send.
unsafe impl Send for Controller {}

impl Controller {
    fn init(&self, ptr: *const nsIWebAuthnController) -> Result<(), nsresult> {
        self.0.replace(ptr);
        Ok(())
    }

    fn finish_register(&self, tid: u64, result: RegisterResultOrError) -> Result<(), nsresult> {
        if (*self.0.borrow()).is_null() {
            return Err(NS_ERROR_FAILURE);
        }
        let wrapped_result = CtapRegisterResult::allocate(InitCtapRegisterResult { result })
            .query_interface::<nsICtapRegisterResult>()
            .ok_or(NS_ERROR_FAILURE)?;
        unsafe {
            (**(self.0.borrow())).FinishRegister(tid, wrapped_result.coerce());
        }
        Ok(())
    }

    fn finish_sign(&self, tid: u64, result: SignResultOrError) -> Result<(), nsresult> {
        if (*self.0.borrow()).is_null() {
            return Err(NS_ERROR_FAILURE);
        }
        let wrapped_result = CtapSignResult::allocate(InitCtapSignResult { result })
            .query_interface::<nsICtapSignResult>()
            .ok_or(NS_ERROR_FAILURE)?;
        unsafe {
            (**(self.0.borrow())).FinishSign(tid, wrapped_result.coerce());
        }
        Ok(())
    }

    fn cancel(&self, tid: u64) -> Result<(), nsresult> {
        if (*self.0.borrow()).is_null() {
            return Err(NS_ERROR_FAILURE);
        }
        unsafe {
            (**(self.0.borrow())).Cancel(tid);
        }
        Ok(())
    }
}

// A transaction may create a channel to ask a user for additional input, e.g. a PIN. The Sender
// component of this channel is sent to an AuthrsTransport in a StatusUpdate. AuthrsTransport
// caches the sender along with the expected (u64) transaction ID, which is used as a consistency
// check in callbacks.
type PinReceiver = Option<(u64, Sender<Pin>)>;
type SelectionReceiver = Option<(u64, Sender<Option<usize>>)>;

fn status_callback(
    status_rx: Receiver<StatusUpdate>,
    tid: u64,
    origin: &String,
    browsing_context_id: u64,
    pin_receiver: Arc<Mutex<PinReceiver>>, /* Shared with an AuthrsTransport */
    selection_receiver: Arc<Mutex<SelectionReceiver>>, /* Shared with an AuthrsTransport */
) -> Result<(), nsresult> {
    let origin = Some(origin.as_str());
    let browsing_context_id = Some(browsing_context_id);
    loop {
        match status_rx.recv() {
            Ok(StatusUpdate::SelectDeviceNotice) => {
                debug!("STATUS: Please select a device by touching one of them.");
                send_prompt(
                    BrowserPromptType::SelectDevice,
                    tid,
                    origin,
                    browsing_context_id,
                )?;
            }
            Ok(StatusUpdate::PresenceRequired) => {
                debug!("STATUS: Waiting for user presence");
                send_prompt(
                    BrowserPromptType::Presence,
                    tid,
                    origin,
                    browsing_context_id,
                )?;
            }
            Ok(StatusUpdate::PinUvError(StatusPinUv::PinRequired(sender))) => {
                pin_receiver.lock().unwrap().replace((tid, sender));
                send_prompt(
                    BrowserPromptType::PinRequired,
                    tid,
                    origin,
                    browsing_context_id,
                )?;
            }
            Ok(StatusUpdate::PinUvError(StatusPinUv::InvalidPin(sender, retries))) => {
                pin_receiver.lock().unwrap().replace((tid, sender));
                send_prompt(
                    BrowserPromptType::PinInvalid { retries },
                    tid,
                    origin,
                    browsing_context_id,
                )?;
            }
            Ok(StatusUpdate::PinUvError(StatusPinUv::PinAuthBlocked)) => {
                send_prompt(
                    BrowserPromptType::PinAuthBlocked,
                    tid,
                    origin,
                    browsing_context_id,
                )?;
            }
            Ok(StatusUpdate::PinUvError(StatusPinUv::PinBlocked)) => {
                send_prompt(
                    BrowserPromptType::DeviceBlocked,
                    tid,
                    origin,
                    browsing_context_id,
                )?;
            }
            Ok(StatusUpdate::PinUvError(StatusPinUv::PinNotSet)) => {
                send_prompt(
                    BrowserPromptType::PinNotSet,
                    tid,
                    origin,
                    browsing_context_id,
                )?;
            }
            Ok(StatusUpdate::PinUvError(StatusPinUv::InvalidUv(retries))) => {
                send_prompt(
                    BrowserPromptType::UvInvalid { retries },
                    tid,
                    origin,
                    browsing_context_id,
                )?;
            }
            Ok(StatusUpdate::PinUvError(StatusPinUv::UvBlocked)) => {
                send_prompt(
                    BrowserPromptType::UvBlocked,
                    tid,
                    origin,
                    browsing_context_id,
                )?;
            }
            Ok(StatusUpdate::PinUvError(StatusPinUv::PinIsTooShort))
            | Ok(StatusUpdate::PinUvError(StatusPinUv::PinIsTooLong(..))) => {
                // These should never happen.
                warn!("STATUS: Got unexpected StatusPinUv-error.");
            }
            Ok(StatusUpdate::InteractiveManagement(_)) => {
                debug!("STATUS: interactive management");
            }
            Ok(StatusUpdate::SelectResultNotice(sender, entities)) => {
                debug!("STATUS: select result notice");
                selection_receiver.lock().unwrap().replace((tid, sender));
                send_prompt(
                    BrowserPromptType::SelectSignResult {
                        entities: &entities,
                    },
                    tid,
                    origin,
                    browsing_context_id,
                )?;
            }
            Err(RecvError) => {
                debug!("STATUS: end");
                break;
            }
        }
    }
    Ok(())
}

enum TransactionArgs {
    Register(/* timeout */ u64, RegisterArgs),
    // Bug 1838932 - we'll need to cache SignArgs once we support conditional mediation
    // Sign(/* timeout */ u64, SignArgs),
}

struct TransactionState {
    tid: u64,
    browsing_context_id: u64,
    pending_args: Option<TransactionArgs>,
}

// AuthrsTransport provides an nsIWebAuthnTransport interface to an AuthenticatorService. This
// allows an nsIWebAuthnController to dispatch MakeCredential and GetAssertion requests to USB HID
// tokens. The AuthrsTransport struct also keeps 1) a pointer to the active nsIWebAuthnController,
// which is used to prompt the user for input and to return results to the controller; and
// 2) a channel through which to receive a pin callback.
#[xpcom(implement(nsIWebAuthnTransport), atomic)]
pub struct AuthrsTransport {
    usb_token_manager: RefCell<StateMachine>, // interior mutable for use in XPCOM methods
    test_token_manager: TestTokenManager,
    controller: Controller,
    pin_receiver: Arc<Mutex<PinReceiver>>,
    selection_receiver: Arc<Mutex<SelectionReceiver>>,
    transaction: Arc<Mutex<Option<TransactionState>>>,
}

impl AuthrsTransport {
    xpcom_method!(get_controller => GetController() -> *const nsIWebAuthnController);
    fn get_controller(&self) -> Result<RefPtr<nsIWebAuthnController>, nsresult> {
        Err(NS_ERROR_NOT_IMPLEMENTED)
    }

    // # Safety
    //
    // This will mutably borrow the controller pointer through a RefCell. The caller must ensure
    // that at most one WebAuthn transaction is active at any given time.
    xpcom_method!(set_controller => SetController(aController: *const nsIWebAuthnController));
    fn set_controller(&self, controller: *const nsIWebAuthnController) -> Result<(), nsresult> {
        self.controller.init(controller)
    }

    xpcom_method!(pin_callback => PinCallback(aTransactionId: u64, aPin: *const nsACString));
    fn pin_callback(&self, transaction_id: u64, pin: &nsACString) -> Result<(), nsresult> {
        let mut guard = self.pin_receiver.lock().or(Err(NS_ERROR_FAILURE))?;
        match guard.take() {
            Some((tid, channel)) if tid == transaction_id => channel
                .send(Pin::new(&pin.to_string()))
                .or(Err(NS_ERROR_FAILURE)),
            // Either we weren't expecting a pin, or the controller is confused
            // about which transaction is active. Neither is recoverable, so it's
            // OK to drop the PinReceiver here.
            _ => Err(NS_ERROR_FAILURE),
        }
    }

    xpcom_method!(selection_callback => SelectionCallback(aTransactionId: u64, aSelection: u64));
    fn selection_callback(&self, transaction_id: u64, selection: u64) -> Result<(), nsresult> {
        let mut guard = self.selection_receiver.lock().or(Err(NS_ERROR_FAILURE))?;
        match guard.take() {
            Some((tid, channel)) if tid == transaction_id => channel
                .send(Some(selection as usize))
                .or(Err(NS_ERROR_FAILURE)),
            // Either we weren't expecting a selection, or the controller is confused
            // about which transaction is active. Neither is recoverable, so it's
            // OK to drop the SelectionReceiver here.
            _ => Err(NS_ERROR_FAILURE),
        }
    }

    // # Safety
    //
    // This will mutably borrow usb_token_manager through a RefCell. The caller must ensure that at
    // most one WebAuthn transaction is active at any given time.
    xpcom_method!(make_credential => MakeCredential(aTid: u64, aBrowsingContextId: u64, aArgs: *const nsICtapRegisterArgs));
    fn make_credential(
        &self,
        tid: u64,
        browsing_context_id: u64,
        args: *const nsICtapRegisterArgs,
    ) -> Result<(), nsresult> {
        self.reset()?;

        if args.is_null() {
            return Err(NS_ERROR_NULL_POINTER);
        }
        let args = unsafe { &*args };

        let mut origin = nsString::new();
        unsafe { args.GetOrigin(&mut *origin) }.to_result()?;

        let mut relying_party_id = nsString::new();
        unsafe { args.GetRpId(&mut *relying_party_id) }.to_result()?;

        let mut client_data_hash = ThinVec::new();
        unsafe { args.GetClientDataHash(&mut client_data_hash) }.to_result()?;
        let mut client_data_hash_arr = [0u8; 32];
        client_data_hash_arr.copy_from_slice(&client_data_hash);

        let mut timeout_ms = 0u32;
        unsafe { args.GetTimeoutMS(&mut timeout_ms) }.to_result()?;

        let mut exclude_list = ThinVec::new();
        unsafe { args.GetExcludeList(&mut exclude_list) }.to_result()?;
        let exclude_list = exclude_list
            .iter_mut()
            .map(|id| PublicKeyCredentialDescriptor {
                id: id.to_vec(),
                transports: vec![],
            })
            .collect();

        let mut relying_party_name = nsString::new();
        unsafe { args.GetRpName(&mut *relying_party_name) }.to_result()?;

        let mut user_id = ThinVec::new();
        unsafe { args.GetUserId(&mut user_id) }.to_result()?;

        let mut user_name = nsString::new();
        unsafe { args.GetUserName(&mut *user_name) }.to_result()?;

        let mut user_display_name = nsString::new();
        unsafe { args.GetUserDisplayName(&mut *user_display_name) }.to_result()?;

        let mut cose_algs = ThinVec::new();
        unsafe { args.GetCoseAlgs(&mut cose_algs) }.to_result()?;
        let pub_cred_params = cose_algs
            .iter()
            .filter_map(|alg| PublicKeyCredentialParameters::try_from(*alg).ok())
            .collect();

        let mut resident_key = nsString::new();
        unsafe { args.GetResidentKey(&mut *resident_key) }.to_result()?;
        let resident_key_req = if resident_key.eq("required") {
            ResidentKeyRequirement::Required
        } else if resident_key.eq("preferred") {
            ResidentKeyRequirement::Preferred
        } else if resident_key.eq("discouraged") {
            ResidentKeyRequirement::Discouraged
        } else {
            return Err(NS_ERROR_FAILURE);
        };

        let mut user_verification = nsString::new();
        unsafe { args.GetUserVerification(&mut *user_verification) }.to_result()?;
        let user_verification_req = if user_verification.eq("required") {
            UserVerificationRequirement::Required
        } else if user_verification.eq("discouraged") {
            UserVerificationRequirement::Discouraged
        } else {
            UserVerificationRequirement::Preferred
        };

        let mut authenticator_attachment = nsString::new();
        if unsafe { args.GetAuthenticatorAttachment(&mut *authenticator_attachment) }
            .to_result()
            .is_ok()
        {
            if authenticator_attachment.eq("platform") {
                return Err(NS_ERROR_FAILURE);
            }
        }

        let mut attestation_conveyance_preference = nsString::new();
        unsafe { args.GetAttestationConveyancePreference(&mut *attestation_conveyance_preference) }
            .to_result()?;
        let none_attestation = !(attestation_conveyance_preference.eq("indirect")
            || attestation_conveyance_preference.eq("direct")
            || attestation_conveyance_preference.eq("enterprise"));

        let mut cred_props = false;
        unsafe { args.GetCredProps(&mut cred_props) }.to_result()?;

        let mut min_pin_length = false;
        unsafe { args.GetMinPinLength(&mut min_pin_length) }.to_result()?;

        // TODO(Bug 1593571) - Add this to the extensions
        // let mut hmac_create_secret = None;
        // let mut maybe_hmac_create_secret = false;
        // match unsafe { args.GetHmacCreateSecret(&mut maybe_hmac_create_secret) }.to_result() {
        //     Ok(_) => hmac_create_secret = Some(maybe_hmac_create_secret),
        //     _ => (),
        // }

        let origin = origin.to_string();
        let info = RegisterArgs {
            client_data_hash: client_data_hash_arr,
            relying_party: RelyingParty {
                id: relying_party_id.to_string(),
                name: None,
            },
            origin: origin.clone(),
            user: PublicKeyCredentialUserEntity {
                id: user_id.to_vec(),
                name: Some(user_name.to_string()),
                display_name: None,
            },
            pub_cred_params,
            exclude_list,
            user_verification_req,
            resident_key_req,
            extensions: AuthenticationExtensionsClientInputs {
                cred_props: Some(cred_props),
                min_pin_length: Some(min_pin_length),
                ..Default::default()
            },
            pin: None,
            use_ctap1_fallback: !static_prefs::pref!("security.webauthn.ctap2"),
        };

        *self.transaction.lock().unwrap() = Some(TransactionState {
            tid,
            browsing_context_id,
            pending_args: Some(TransactionArgs::Register(timeout_ms as u64, info)),
        });

        if none_attestation
            || static_prefs::pref!("security.webauth.webauthn_testing_allow_direct_attestation")
        {
            // TODO(Bug 1855290) Remove this presence prompt
            send_prompt(
                BrowserPromptType::Presence,
                tid,
                Some(&origin),
                Some(browsing_context_id),
            )?;
            self.resume_make_credential(tid, none_attestation)
        } else {
            send_prompt(
                BrowserPromptType::RegisterDirect,
                tid,
                Some(&origin),
                Some(browsing_context_id),
            )?;
            Ok(())
        }
    }

    xpcom_method!(resume_make_credential => ResumeMakeCredential(aTid: u64, aForceNoneAttestation: bool));
    fn resume_make_credential(
        &self,
        tid: u64,
        force_none_attestation: bool,
    ) -> Result<(), nsresult> {
        let mut guard = self.transaction.lock().unwrap();
        let Some(state) = guard.as_mut() else {
            return Err(NS_ERROR_FAILURE);
        };
        if state.tid != tid {
            return Err(NS_ERROR_FAILURE);
        };
        let browsing_context_id = state.browsing_context_id;
        let (timeout_ms, info) = match state.pending_args.take() {
            Some(TransactionArgs::Register(timeout_ms, info)) => (timeout_ms, info),
            _ => return Err(NS_ERROR_FAILURE),
        };

        let (status_tx, status_rx) = channel::<StatusUpdate>();
        let pin_receiver = self.pin_receiver.clone();
        let selection_receiver = self.selection_receiver.clone();
        let status_origin = info.origin.clone();
        RunnableBuilder::new(
            "AuthrsTransport::MakeCredential::StatusReceiver",
            move || {
                let _ = status_callback(
                    status_rx,
                    tid,
                    &status_origin,
                    browsing_context_id,
                    pin_receiver,
                    selection_receiver,
                );
            },
        )
        .may_block(true)
        .dispatch_background_task()?;

        let controller = self.controller.clone();
        let callback_origin = info.origin.clone();
        let state_callback = StateCallback::<RegisterResultOrError>::new(Box::new(move |result| {
            let result = match result {
                Ok(mut make_cred_res) => {
                    // Tokens always provide attestation, but the user may have asked we not
                    // include the attestation statement in the response.
                    if force_none_attestation {
                        make_cred_res.att_obj.anonymize();
                    }
                    Ok(make_cred_res)
                }
                Err(e @ AuthenticatorError::CredentialExcluded) => {
                    let _ = send_prompt(
                        BrowserPromptType::AlreadyRegistered,
                        tid,
                        Some(&callback_origin),
                        Some(browsing_context_id),
                    );
                    Err(e)
                }
                Err(e) => Err(e),
            };
            // Some errors are accompanied by prompts that should persist after the
            // operation terminates.
            if result.is_ok() || error_cancels_prompts(&result.as_ref().unwrap_err()) {
                let _ = cancel_prompts(tid);
            }
            let _ = controller.finish_register(tid, result);
        }));

        // The authenticator crate provides an `AuthenticatorService` which can dispatch a request
        // in parallel to any number of transports. We only support the USB transport in production
        // configurations, so we do not need the full generality of `AuthenticatorService` here.
        // We disable the USB transport in tests that use virtual devices.
        if static_prefs::pref!("security.webauth.webauthn_enable_usbtoken") {
            self.usb_token_manager.borrow_mut().register(
                timeout_ms,
                info.into(),
                status_tx,
                state_callback,
            );
        } else if static_prefs::pref!("security.webauth.webauthn_enable_softtoken") {
            self.test_token_manager
                .register(timeout_ms, info.into(), status_tx, state_callback);
        } else {
            return Err(NS_ERROR_FAILURE);
        }

        Ok(())
    }

    // # Safety
    //
    // This will mutably borrow usb_token_manager through a RefCell. The caller must ensure that at
    // most one WebAuthn transaction is active at any given time.
    xpcom_method!(get_assertion => GetAssertion(aTid: u64, aBrowsingContextId: u64, aArgs: *const nsICtapSignArgs));
    fn get_assertion(
        &self,
        tid: u64,
        browsing_context_id: u64,
        args: *const nsICtapSignArgs,
    ) -> Result<(), nsresult> {
        self.reset()?;

        if args.is_null() {
            return Err(NS_ERROR_NULL_POINTER);
        }
        let args = unsafe { &*args };

        let mut origin = nsString::new();
        unsafe { args.GetOrigin(&mut *origin) }.to_result()?;

        let mut relying_party_id = nsString::new();
        unsafe { args.GetRpId(&mut *relying_party_id) }.to_result()?;

        let mut client_data_hash = ThinVec::new();
        unsafe { args.GetClientDataHash(&mut client_data_hash) }.to_result()?;
        let mut client_data_hash_arr = [0u8; 32];
        client_data_hash_arr.copy_from_slice(&client_data_hash);

        let mut timeout_ms = 0u32;
        unsafe { args.GetTimeoutMS(&mut timeout_ms) }.to_result()?;

        let mut allow_list = ThinVec::new();
        unsafe { args.GetAllowList(&mut allow_list) }.to_result()?;
        let allow_list: Vec<_> = allow_list
            .iter_mut()
            .map(|id| PublicKeyCredentialDescriptor {
                id: id.to_vec(),
                transports: vec![],
            })
            .collect();

        let mut user_verification = nsString::new();
        unsafe { args.GetUserVerification(&mut *user_verification) }.to_result()?;
        let user_verification_req = if user_verification.eq("required") {
            UserVerificationRequirement::Required
        } else if user_verification.eq("discouraged") {
            UserVerificationRequirement::Discouraged
        } else {
            UserVerificationRequirement::Preferred
        };

        let mut app_id = None;
        let mut maybe_app_id = nsString::new();
        match unsafe { args.GetAppId(&mut *maybe_app_id) }.to_result() {
            Ok(_) => app_id = Some(maybe_app_id.to_string()),
            _ => (),
        }

        let (status_tx, status_rx) = channel::<StatusUpdate>();
        let pin_receiver = self.pin_receiver.clone();
        let selection_receiver = self.selection_receiver.clone();
        let status_origin = origin.to_string();
        RunnableBuilder::new("AuthrsTransport::GetAssertion::StatusReceiver", move || {
            let _ = status_callback(
                status_rx,
                tid,
                &status_origin,
                browsing_context_id,
                pin_receiver,
                selection_receiver,
            );
        })
        .may_block(true)
        .dispatch_background_task()?;

        let uniq_allowed_cred = if allow_list.len() == 1 {
            allow_list.first().cloned()
        } else {
            None
        };

        let controller = self.controller.clone();
        let state_callback =
            StateCallback::<SignResultOrError>::new(Box::new(move |mut result| {
                if uniq_allowed_cred.is_some() {
                    // In CTAP 2.0, but not CTAP 2.1, the assertion object's credential field
                    // "May be omitted if the allowList has exactly one credential." If we had
                    // a unique allowed credential, then copy its descriptor to the output.
                    if let Ok(inner) = result.as_mut() {
                        inner.assertion.credentials = uniq_allowed_cred;
                    }
                }
                // Some errors are accompanied by prompts that should persist after the
                // operation terminates.
                if result.is_ok() || error_cancels_prompts(&result.as_ref().unwrap_err()) {
                    let _ = cancel_prompts(tid);
                }
                let _ = controller.finish_sign(tid, result);
            }));

        let info = SignArgs {
            client_data_hash: client_data_hash_arr,
            relying_party_id: relying_party_id.to_string(),
            origin: origin.to_string(),
            allow_list,
            user_verification_req,
            user_presence_req: true,
            extensions: AuthenticationExtensionsClientInputs {
                app_id,
                ..Default::default()
            },
            pin: None,
            use_ctap1_fallback: !static_prefs::pref!("security.webauthn.ctap2"),
        };

        // TODO(Bug 1855290) Remove this presence prompt
        send_prompt(
            BrowserPromptType::Presence,
            tid,
            Some(&info.origin),
            Some(browsing_context_id),
        )?;

        *self.transaction.lock().unwrap() = Some(TransactionState {
            tid,
            browsing_context_id,
            pending_args: None,
        });

        // As in `register`, we are intentionally avoiding `AuthenticatorService` here.
        if static_prefs::pref!("security.webauth.webauthn_enable_usbtoken") {
            self.usb_token_manager.borrow_mut().sign(
                timeout_ms as u64,
                info.into(),
                status_tx,
                state_callback,
            );
        } else if static_prefs::pref!("security.webauth.webauthn_enable_softtoken") {
            self.test_token_manager
                .sign(timeout_ms as u64, info.into(), status_tx, state_callback);
        } else {
            return Err(NS_ERROR_FAILURE);
        }

        Ok(())
    }

    xpcom_method!(cancel => Cancel(aTransactionId: u64));
    fn cancel(&self, tid: u64) -> Result<(), nsresult> {
        let mut guard = self.transaction.lock().unwrap();
        if guard.as_ref().map_or(false, |state| state.tid == tid) {
            self.reset_helper()?;
            self.controller.cancel(tid)?;
            *guard = None;
        }
        Ok(())
    }

    xpcom_method!(reset => Reset());
    fn reset(&self) -> Result<(), nsresult> {
        if let Some(transaction) = self.transaction.lock().unwrap().take() {
            self.reset_helper()?;
            cancel_prompts(transaction.tid)?;
        }
        Ok(())
    }

    fn reset_helper(&self) -> Result<(), nsresult> {
        drop(self.pin_receiver.lock().or(Err(NS_ERROR_FAILURE))?.take());
        drop(
            self.selection_receiver
                .lock()
                .or(Err(NS_ERROR_FAILURE))?
                .take(),
        );
        self.usb_token_manager.borrow_mut().cancel();
        Ok(())
    }

    xpcom_method!(
        add_virtual_authenticator => AddVirtualAuthenticator(
            protocol: *const nsACString,
            transport: *const nsACString,
            hasResidentKey: bool,
            hasUserVerification: bool,
            isUserConsenting: bool,
            isUserVerified: bool) -> u64
    );
    fn add_virtual_authenticator(
        &self,
        protocol: &nsACString,
        transport: &nsACString,
        has_resident_key: bool,
        has_user_verification: bool,
        is_user_consenting: bool,
        is_user_verified: bool,
    ) -> Result<u64, nsresult> {
        let protocol = match protocol.to_string().as_str() {
            "ctap1/u2f" => AuthenticatorVersion::U2F_V2,
            "ctap2" => AuthenticatorVersion::FIDO_2_0,
            "ctap2_1" => AuthenticatorVersion::FIDO_2_1,
            _ => return Err(NS_ERROR_INVALID_ARG),
        };
        match transport.to_string().as_str() {
            "usb" | "nfc" | "ble" | "smart-card" | "hybrid" | "internal" => (),
            _ => return Err(NS_ERROR_INVALID_ARG),
        };
        self.test_token_manager.add_virtual_authenticator(
            protocol,
            has_resident_key,
            has_user_verification,
            is_user_consenting,
            is_user_verified,
        )
    }

    xpcom_method!(remove_virtual_authenticator => RemoveVirtualAuthenticator(authenticatorId: u64));
    fn remove_virtual_authenticator(&self, authenticator_id: u64) -> Result<(), nsresult> {
        self.test_token_manager
            .remove_virtual_authenticator(authenticator_id)
    }

    xpcom_method!(
        add_credential => AddCredential(
            authenticatorId: u64,
            credentialId: *const nsACString,
            isResidentCredential: bool,
            rpId: *const nsACString,
            privateKey: *const nsACString,
            userHandle: *const nsACString,
            signCount: u32)
    );
    fn add_credential(
        &self,
        authenticator_id: u64,
        credential_id: &nsACString,
        is_resident_credential: bool,
        rp_id: &nsACString,
        private_key: &nsACString,
        user_handle: &nsACString,
        sign_count: u32,
    ) -> Result<(), nsresult> {
        let credential_id = base64::engine::general_purpose::URL_SAFE_NO_PAD
            .decode(credential_id)
            .or(Err(NS_ERROR_INVALID_ARG))?;
        let private_key = base64::engine::general_purpose::URL_SAFE_NO_PAD
            .decode(private_key)
            .or(Err(NS_ERROR_INVALID_ARG))?;
        let user_handle = base64::engine::general_purpose::URL_SAFE_NO_PAD
            .decode(user_handle)
            .or(Err(NS_ERROR_INVALID_ARG))?;
        self.test_token_manager.add_credential(
            authenticator_id,
            &credential_id,
            &private_key,
            &user_handle,
            sign_count,
            rp_id.to_string(),
            is_resident_credential,
        )
    }

    xpcom_method!(get_credentials => GetCredentials(authenticatorId: u64) -> ThinVec<Option<RefPtr<nsICredentialParameters>>>);
    fn get_credentials(
        &self,
        authenticator_id: u64,
    ) -> Result<ThinVec<Option<RefPtr<nsICredentialParameters>>>, nsresult> {
        self.test_token_manager.get_credentials(authenticator_id)
    }

    xpcom_method!(remove_credential => RemoveCredential(authenticatorId: u64, credentialId: *const nsACString));
    fn remove_credential(
        &self,
        authenticator_id: u64,
        credential_id: &nsACString,
    ) -> Result<(), nsresult> {
        let credential_id = base64::engine::general_purpose::URL_SAFE_NO_PAD
            .decode(credential_id)
            .or(Err(NS_ERROR_INVALID_ARG))?;
        self.test_token_manager
            .remove_credential(authenticator_id, credential_id.as_ref())
    }

    xpcom_method!(remove_all_credentials => RemoveAllCredentials(authenticatorId: u64));
    fn remove_all_credentials(&self, authenticator_id: u64) -> Result<(), nsresult> {
        self.test_token_manager
            .remove_all_credentials(authenticator_id)
    }

    xpcom_method!(set_user_verified => SetUserVerified(authenticatorId: u64, isUserVerified: bool));
    fn set_user_verified(
        &self,
        authenticator_id: u64,
        is_user_verified: bool,
    ) -> Result<(), nsresult> {
        self.test_token_manager
            .set_user_verified(authenticator_id, is_user_verified)
    }
}

#[no_mangle]
pub extern "C" fn authrs_transport_constructor(
    result: *mut *const nsIWebAuthnTransport,
) -> nsresult {
    let wrapper = AuthrsTransport::allocate(InitAuthrsTransport {
        usb_token_manager: RefCell::new(StateMachine::new()),
        test_token_manager: TestTokenManager::new(),
        controller: Controller(RefCell::new(std::ptr::null())),
        pin_receiver: Arc::new(Mutex::new(None)),
        selection_receiver: Arc::new(Mutex::new(None)),
        transaction: Arc::new(Mutex::new(None)),
    });

    #[cfg(feature = "fuzzing")]
    {
        let fuzzing_config = static_prefs::pref!("fuzzing.webauthn.authenticator_config");
        if fuzzing_config != 0 {
            let is_user_verified = (fuzzing_config & 0x01) != 0;
            let is_user_consenting = (fuzzing_config & 0x02) != 0;
            let has_user_verification = (fuzzing_config & 0x04) != 0;
            let has_resident_key = (fuzzing_config & 0x08) != 0;
            let transport = nsCString::from(match (fuzzing_config & 0x10) >> 4 {
                0 => "usb",
                1 => "internal",
                _ => unreachable!(),
            });
            let protocol = nsCString::from(match (fuzzing_config & 0x60) >> 5 {
                0 => "", // reserved
                1 => "ctap1/u2f",
                2 => "ctap2",
                3 => "ctap2_1",
                _ => unreachable!(),
            });
            // If this fails it's probably because the protocol bits were zero,
            // we'll just ignore it.
            let _ = wrapper.add_virtual_authenticator(
                &protocol,
                &transport,
                has_resident_key,
                has_user_verification,
                is_user_consenting,
                is_user_verified,
            );
        }
    }

    unsafe {
        RefPtr::new(wrapper.coerce::<nsIWebAuthnTransport>()).forget(&mut *result);
    }
    NS_OK
}

#[no_mangle]
pub extern "C" fn authrs_webauthn_att_obj_constructor(
    att_obj_bytes: &ThinVec<u8>,
    anonymize: bool,
    result: *mut *const nsIWebAuthnAttObj,
) -> nsresult {
    if result.is_null() {
        return NS_ERROR_NULL_POINTER;
    }

    let mut att_obj: AttestationObject = match serde_cbor::from_slice(att_obj_bytes) {
        Ok(att_obj) => att_obj,
        Err(_) => return NS_ERROR_INVALID_ARG,
    };

    if anonymize {
        att_obj.anonymize();
    }

    let wrapper = WebAuthnAttObj::allocate(InitWebAuthnAttObj { att_obj });

    unsafe {
        RefPtr::new(wrapper.coerce::<nsIWebAuthnAttObj>()).forget(&mut *result);
    }

    NS_OK
}
